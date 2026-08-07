# RECORD13 — crtproc 多进程调度 + 两个 Bug 修复(switch_to 不保存 RBP / SLOT_SIZE 超出内存)

## 背景:crtproc 多进程工作(未提交)

RECORD12.2 之后完成了一套多进程/用户 shell 工作(设计见 `DESIGN_CRTPROC.md`),本次会话在其之上修复了两个 bug:

- `SYS_CRTPROC`(spawn 进程)/ `SYS_EXIT` 系统调用
- `kernel/process.c` / `process.h` — 16 槽位进程表、时间片调度器(`schedule`/`sched_tick`)、内核栈池(`kstack_alloc`/`kstack_free`)
- `kernel/paging.c` / `paging.h` — 每进程独立页表(PML4+PDP+PD 池,按 pid 分 60MB 物理槽位),`pt_create`/`pt_destroy`
- `arch/user.S` — `switch_to`(切 RSP/TSS.rsp0/CR3)与 `resume_user`(首入进程的 iretq 蹦床)
- `kernel/programs/ush.c` — ring-3 用户 shell,`run <prog>` 通过 `crtproc()` 派生子进程
- 串口(COM1)作为 dd=2 输出设备,ring3.c 的 "hello" 走串口便于调试

## Bug 1 — switch_to 不保存/恢复 RBP,子进程退出后系统卡死,PID 不打印

### 症状

在 ush 里输入 `run ring3`:

```
$ run ring3
spawn ring3:           ← 打印到这里就卡死
```

串口里能看到 "hello, world from ring 3!"(子进程成功跑完了它的 output 系统调用),但 ush 永远没有恢复,`pid 2` 不打印,系统冻结。

### 预期流程

```
ush(pid 1) --crtproc--> 子进程(pid 2) --exit--> 切回 ush,打印 "pid 2"
```

`crtproc` 系统调用内部发生一次抢占式切换(irq_handler 里 `need_resched` → `schedule` → `switch_to`),子进程退出时再切回 ush。ush 恢复后从 `int $0x80` 之后继续,打印 PID。

### 根因:switch_to 只切了栈,没切帧指针

`switch_to`(arch/user.S)只做了三件事:保存/恢复 `k_rsp`、更新 TSS.rsp0、换 CR3。**它没有保存/恢复 RBP。**

x86-64 C 函数(未开 `-fomit-frame-pointer`)用 RBP 做帧指针,函数结尾的 `leave`(即 `mov rsp, rbp; pop rbp`)依赖 RBP 指向**自己**的栈帧。跨进程恢复时,恢复的只是 RSP,而 RBP 仍然是**上一个进程**的帧指针:

```
子进程 exit 路径(子进程内核栈 0x16000-0x17000):
  proc_exit_current ─► schedule ─► switch_to(ush)
                                     │
             保存子进程 k_rsp = 0x16ea8          ✓
             TSS.rsp0 = ush.k_stack_top          ✓
             CR3     = ush 的页表                ✓
             RSP     = ush.k_rsp = 0x15ee8       ✓ (ush 栈上, schedule 内)
             RBP     = ???  ← 仍是子进程 proc_exit_current 的帧指针 0x16ef0 ✗
             ret → 弹回 schedule 里 call switch_to 的返回地址(0xb6d6)
             │
ush 在 schedule 中恢复执行:
             current->state = PROC_RUNNING
             sched_ticks    = SCHED_QUANTUM
             leave          ← mov rsp, rbp → RSP 被拽回 0x16ef0(子进程的栈!)
                              pop rbp      → 弹回子进程 exit 路径的调用链
             ret            → 沿子进程的栈返回:syscall_handler → common_stub → iretq
             │
iretq 把 ZOMBIE 子进程(rip=0x300023, rsp=0x600000)送回 ring 3 —— 但它已退出、
页表已 pt_destroy, 且当前 CR3 是 ush 的 —— 恢复链彻底混乱
```

最终 CPU 停在子进程第一次 `proc_exit_current` 的 `for(;;)` 死循环里(它在子进程栈上被"返回"到了那里)。

### GDB 证据

用 QEMU `-s` + GDB 断点链追踪(proc_exit_current / schedule / switch_to / resume_user 打点):

```
>>> switch_to entry rsp=15ee8 current=130b8 cur_pid=1 next=13170 next_pid=2   ← crtproc 切换: 保存 ush.k_rsp=0x15ee8 ✓
>>> resume_user rsp=16f50 current=13170                                        ← 子进程首入
>>> proc_exit_current rsp=16ef8 current=13170 cur_pid=2 cur_state=3            ← 子进程 exit(已是 ZOMBIE)
>>> schedule entry rsp=16ed8 current=13170 cur_pid=2 cur_state=3
>>> switch_to entry rsp=16ea8 current=13170 next=130b8 next_pid=1              ← exit 切换: 读到的 ush.k_rsp=0x15ee8 ✓(值没错!)
!!! HANG at exit-for(;;) rsp=16ee0 cr3=25000                                   ← 但 ush 没恢复, 卡死在子进程栈上
```

关键点:exit 切换时 ush.k_rsp 是**正确的**(0x15ee8,ush 栈上,里面也确实有 schedule 的返回地址),但 ush 恢复后 `leave` 用了子进程的 RBP,把栈拽回子进程侧——这正是 RBP 缺失的铁证。

卡死期间定时器(环 0 同栈触发,不换栈)反复执行 `schedule → switch_to(ush)` 自切换,`rsp=0x16dc8`(在子进程栈上)反复覆盖 `ush.k_rsp`,让现场更难读——这也是为什么一开始以为是 k_rsp 被写坏。

### 为什么定时器自切换一直正常(掩盖了 bug)

定时器抢占 ush 时,`irq_handler → schedule → switch_to(ush)` 的 RBP 属于**同一个调用链**,`leave` 弹回的是自己帧的 RBP,恢复完全正确。只有**跨进程**切换才暴露 RBP 缺失,而跨进程切换在 crtproc 之前从未发生过(之前的 enter_user_mode 是单向的)。

### 修复

1. `kernel/process.h` — `struct process` 增加 `k_rbp` 字段(放在 cr3 之后,偏移 160;k_rsp@136、k_stack_top@144、cr3@152 不变,user.S 硬编码偏移不受影响):

```c
uint64_t      cr3;          /* physical address of PML4 */
uint64_t      k_rbp;        /* saved RBP at switch time */
```

2. `arch/user.S` — `switch_to` 保存/恢复 RBP:

```asm
switch_to:
    movq    current(%rip), %rax
    movq    %rsp, 136(%rax)
    movq    %rbp, 160(%rax)        # 新增: 保存 RBP
    ...
    movq    136(%rdi), %rsp
    movq    160(%rdi), %rbp        # 新增: 恢复 RBP
    ...
```

3. `kernel/process.c` — `process_init` 与 `proc_spawn` 里 `k_rbp = 0`(新进程首次进入走 `resume_user`,会从伪造帧弹出 rbp=0,初始值无所谓)。

## Bug 2 — SLOT_SIZE 实际是 120MB,pid 3 的槽位超出 128MB RAM,子进程 PF@0

### 症状

修好 Bug 1 后第一次 spawn 正常(`spawn ring3: pid 2`),**第二次** spawn 的子进程崩溃:

```
$ run ring3
spawn ring3: pid 2
$ run ring3
spawn ring3: pid 3 PF @ 00pid 3
```

"PF @ 00" 是页故障处理打印的 CR2(高 32 位 "0" + 低 32 位 "0")——故障地址是 **0**。

### 根因:注释与代码不符,槽位大小算错一倍

`paging.h`:

```c
#define SLOT_SIZE       (60ULL * 0x200000)   /* 60 MB (30 × 2 MB huge pages) */
```

注释说 60MB(30 个 2MB 页),但代码 `60 × 0x200000 = 120MB`。于是:

| pid | SLOT_BASE = 0x200000 + (pid-1)×120MB | 是否在 128MB RAM 内 |
|-----|--------------------------------------|---------------------|
| 1 (ush) | 0x200000 | ✓ |
| 2 | 0x7A00000 (122MB) | ✓(所以第一次 spawn 正常) |
| 3 | 0xF200000 (242MB) | ✗ 超出! |
| 4 | 0x12A00000 (466MB) | ✗ |

子进程的代码被拷贝到 0xF300000(243MB),但 **QEMU 对超出 RAM 的物理地址:读返回全零,写被丢弃**——代码等于没拷进去。子进程 iretq 到 VA 0x300000,该页映射到 0xF300000,取指得到全零页,CPU 把全零当指令执行:`add [rax], al` 一条接一条,rax=0(伪造帧里 GPR 全零)→ 访问 VA 0 → #PF,CR2=0。

### GDB/QEMU 证据

1. **PF 现场**(正确从 `$rdi`(struct registers 指针)读字段):

```
PF cr2=0 err=5 rip=300000 cs=1b rflags=202 rsp=600000
```

err=5 = P=1(页存在)+ U/S=1(用户态)+ 数据读(非取指)。RIP=0x300000 但 CR2=0——典型"执行到全零页,`add [rax],al` 访问了 rax(=0)"的特征。

2. **子进程页表 dump**(池 idx1,0x28000):

```
pd[0]=0xe3        ← 内核页(0-2MB, 有 A/D 位)
pd[1]=0xf200087   ← VA 0x200000-0x400000 → phys 0xf200000 (242MB!) 超出 RAM
pd[2]=0xf400087   ← VA 0x400000-0x600000 → phys 0xf400000 (244MB!) 超出 RAM
```

3. **pt_create 断点**——两次 spawn 的 slot_base 正好是 SLOT_BASE(2)/(3):

```
>>> proc_spawn name=ring3
>>> pt_create slot_base=7a00000 user_pages=2   ← 第一次 spawn (pid 2)
>>> proc_spawn name=ring3
>>> pt_create slot_base=f200000 user_pages=2   ← 第二次 spawn (pid 3)
```

4. 物理内存对比:pid 3 槽位的文本页 0x7B00000 内容完好(代码确实拷过去了),但执行的是零——因为页表把 VA 0x300000 指到了超内存的 0xF300000。

### 修复

`paging.h` 按注释修正为 60MB(30 × 2MB):

```c
#define SLOT_SIZE       (30ULL * 0x200000)   /* 60 MB (30 × 2 MB huge pages) */
```

修复后 pid 3 槽位 = 0x7A00000(122MB),在 RAM 内。

## 调试方法记录(QEMU QMP + GDB,可复用)

- **无头运行**:`qemu-system-x86_64 -display none -qmp unix:/tmp/qmp.sock,server,nowait -serial file:/tmp/serial.out -s`
- **QMP 自动驱动**:python unix socket 连接,`qmp_capabilities` 握手后 `send-key`(qcode 序列)自动敲 `run ring3`,`pmemsave 0xB8000 4000 /tmp/vga.bin` dump VGA 文本缓冲区,按"每字符 2 字节(字符+属性)、每行 80 字符"解码成屏幕文本——无需截图
- **GDB**:`-s` 开 1234 端口,`target remote :1234`,对内核 ELF(`stage2.elf`,无 -g 调试信息)用**地址断点** + `commands` 打点,打印 `current`/`pid`/`state`/`rsp`/`next` 等 PCB 字段;PF handler 断点处注意 **rsp 不是 struct registers 基址,要从第一个参数 `$rdi` 偏移读**(err@128、rip@136、cs@144)
- 每次 WSL 调用间 qemu 会消失,把"启动 qemu + 驱动 + gdb"合并到同一条命令里执行

## 验证

```
$ run ring3          $ run ring3
spawn ring3: pid 2   spawn ring3: pid 3
$                    $
```

- VGA:两次 spawn 都打印 PID,shell 每次都回到 `$` 提示符
- 串口:两条 "hello, world from ring 3!"(两个子进程都完整执行了自己的代码)
- 循环 3 次(每次启动全新 qemu、跑两次 spawn)均稳定

## 文件清单

| 文件 | 修改 |
|------|------|
| `kernel/process.h` | `struct process` 增加 `k_rbp` 字段 |
| `arch/user.S` | `switch_to` 保存/恢复 RBP |
| `kernel/process.c` | `process_init`/`proc_spawn` 初始化 `k_rbp = 0` |
| `kernel/paging.h` | `SLOT_SIZE` 60×2MB → 30×2MB(120MB → 60MB) |

## 已知限制

- **内存上限**:60MB/槽位 × pid 的设计下,128MB RAM 最多同时 3 个用户进程(pid 1-3);pid 4 的槽位(182MB)会再次超界。需要更多进程须加大 QEMU 内存(`-m`)
- **Zombie 不回收**:没有 wait/reap,退出进程的 PCB 槽位永远是 ZOMBIE,PID 不复用
- `pt_destroy` 在换页表前释放了当前进程的页表池槽位(内存还在,只是归还),顺序上恰好安全——`switch_to` 在 `ret` 之前就加载了新 CR3,值得注意但暂不影响正确性
- `proc_exit_current` 里 `schedule()` 之后的 `sti; for(;;)` 是理论死路:ZOMBIE 不会被调度,正常流程永远不会走到
