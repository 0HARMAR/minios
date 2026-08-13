# RECORD14 — 键盘终端所有权 + 用户程序全面 syscall 化(INPUT 权限隔离)

## 背景:INPUT 系统调用无权限控制

键盘缓冲(`drivers/keyboard.c` 的全局环形队列)是共享的,`sys_input(DD_KEYBOARD, ...)` 只校验用户缓冲区地址合法,不校验"谁有权读键盘"。任何进程——ush(pid 1)或任意用户进程——都能调用 `input(1, ...)` 消费同一个缓冲区。后果:**用户 shell 里敲的字符可能被后台进程抢走**。

## 设计:惰性键盘所有权(Lazy Keyboard Ownership)

等价于 Unix 前台进程组的极简版。核心规则:**键盘同一时刻只属于一个进程;归属在"真正读键盘"的那一刻决定,而不是在 spawn 时分配。**

### 规则

1. **惰性获取**:进程第一次调用 `input(DD_KEYBOARD)` 时,若键盘空闲(`kbd_owner == 0`)或已归自己,则自动成为 owner。**不需要键盘的进程(primes/fib/net...)从不读键盘,永不占用。**
2. **占用拒绝**:键盘被其他进程持有时,读取请求返回 `-EBUSY`(新错误码),**不偷字符、不阻塞**(空转重试,等价于简化版 wait)。
3. **spawn 让出**:`crtproc` 成功创建进程后,调用者(通常是 ush)主动 `keyboard_release`——交互程序首读即可拿到键盘,不会因"ush 永不退出"而永久饥饿。
4. **退出归还**:进程退出/被杀(PF 等)时 `keyboard_release(current->pid)`,键盘自动回到空闲。

### 场景推演

| 场景 | 行为 |
|------|------|
| ush 持有键盘,`run calc` | spawn 让出 → calc 首次 `input` 惰性拿到 ✓ |
| calc 退出 | release → 空闲 → ush 恢复输入 ✓ |
| `run primes`(不读键盘) | ush 让出后键盘空闲,ush 下一轮 input 抢回 ✓ |
| calc 持有期间 ush 输入 | `-EBUSY`,ush 静默重试(等前台进程退出) |

### 检查点为什么放在驱动层

用户程序中有两类键盘读取路径:

- **syscall 路径**(ush/ring3):`input(1, ...)` → 可在 `sys_input` 层拦截
- **直接调用路径**(echo/calc/guess 旧版):`--defsym=keyboard_read=内核地址`,ring3 直接调用内核函数

直接调用路径绕过了 syscall,因此所有权检查必须下沉到 `keyboard_read()` 驱动层(非 owner 返回 0),syscall 层额外给明确的 `-EBUSY` 语义。**本次同时把直接调用路径全部消灭**(见下节),检查点归一。

## Bug 1 — 用户程序直接链接内核符号,ring3 访问全部 PF 被杀

### 症状

`run calc` / `run guess` / `run echo`(以及 hello/primes/fib/chars/net)在 spawn 后立即:

```
spawn calc: pid 2 PF @ 081988pid 2
```

### 根因:内核页表没有 USER 位,ring3 无法访问任何内核符号

- `build_64_boot.sh` 用 `--defsym=keyboard_read=$P_KR` 等把内核函数地址硬链接进用户程序
- 但用户页表 `PD[0]`(VA 0-2MB,内核区)来自 boot 页表,**supervisor-only,无 `PAGE_USER`**(`boot_64.S` PD[0] 填充、`paging.c` `new_pd[0] = pd[0]`)
- ring3 一调用 `print_string`/`keyboard_read`(地址都在 0x8000-0x16000)→ 立即 #PF → `page_fault_handler` 杀进程

也就是说:这些程序在 ring3 下**从来就没有跑起来过**,一直靠"直接调内核函数"的陈旧写法掩盖着。

### 修复:用户程序全面 syscall 化

8 个程序(hello/echo/primes/fib/chars/calc/guess/net)全部改为纯 syscall:

- `#include "../userlib.h"`(与 ush 一致),`output(0, str, len)` 代替 VGA 直写
- `input(1, &c, 1)` 读键盘,返回值 `< 0` 时静默重试(键盘被占)
- 删掉 `VGA_BUFFER`/`cursor`/`vga_cursor_pos`/`update_cursor` 等所有 VGA 操作(光标由内核 `vga_write` 统一管理),backspace 用 `"\b \b"` 序列(ush 同款)
- `build_64_boot.sh` — `build_program` 删除全部内核符号 `--defsym`(仅保留 `BASE`)

net.c 本就纯 syscall(唯一当时能跑的程序),仅验证。

## Bug 2 — exec_init 的 BSS 清零破坏下一个程序头部(spawn ush failed)

### 症状

给 echo/calc/guess 新增 `static char putc_buf` 等全局变量后,`prog_count` 扫描中断:

```
MiniOS 64-bit Shell
[e1000 ready] ...
spawn ush failed          ← 程序表里找不到 ush,exec_lookup 返回 NULL
```

### 根因:程序的 BSS 在内存布局上与下一个程序的 a.out 头重叠

a.out 格式 BSS 无文件空间(`A_FILESZ` 不含 bss),程序按 `header + text + data` 连续排列,下一个程序的头紧跟其后。`exec_init` Phase 3 对每个程序"zero-fill BSS"(在 PROGRAMS_BASE 镜像的 `header+text+data` 之后)——

```
echo:  bss=1(putc_buf)  → 清零 1 字节,正好抹掉 primes 的 a_magic 最低字节
```

扫描在 primes 处 `magic != AOUT_MAGIC` 直接 break,primes 及之后全部程序(含 ush)从程序表消失。旧程序 bss=0,所以从未触发;RECORD12.2 的"guess a_bss=4 破坏 ring3 头"是同一 bug 的另一实例(当时靠调整程序构建顺序规避)。

### 修复(根治)

`kernel/exec.c` — 删除 Phase 3 的 BSS 清零:

```c
/* BSS is NOT zeroed here — it would clobber the next program's header.
   proc_spawn zeroes BSS in the per-process slot instead. */
```

`proc_spawn`(`process.c`)拷贝 text+data 到 pid 槽位后**本来就对 slot 布局的 BSS 清零**,`exec()`/`exec_user()` 是死代码——清零在 exec_init 里是多余且有害的。构建顺序 hack 不再需要。

## Bug 3 — 程序结束后缺 exit(0),RIP 落入垃圾地址 PF

### 症状

hello/primes 打印完输出后:

```
Hello from program!  -> ran via MINI exec formatpid 2 PF @ 06291456pid 2
```

### 根因

用户程序 `_start` 是裸入口(无 libc 返回链),函数结束后 RIP 落到伪造帧残留的垃圾地址。PF @ 0x6291456 是栈上残留值(不是固定规律,取决于现场)。

### 修复

7 个程序末尾统一补 `exit(0)`(userlib.h 的 noreturn syscall)。

## 环境注意(QEMU 内存,非代码 bug)

pid ≥ 4 的槽位 `SLOT_BASE(4) = 0xB600000`(182MB)超出 QEMU 默认 128MB RAM(写被丢弃 → 取指全零页 → `add [rax], al` 链 → CR2=0 的 PF@0,与 RECORD13 Bug 2 同机理)。**测试必须 `-m 512`。**

```bash
qemu-system-x86_64 -m 512 -hda long-mode/output/disk.img
```

## 验证

`-m 512` 下连续 spawn 全量程序:

```
run hello   → Hello from program!  ...  pid 2 正常退出
run primes  → 95 个素数完整输出        pid 3 正常退出
run fib     → F(0)..F(29) 完整          pid 4 正常退出
run chars   → ASCII 表完整              pid 5 正常退出
run calc    → 5 + 3 计算、q 退出        pid 6
run echo    → "hello world" 回显、空行退出 pid 7
run guess   → 猜数字循环(50 high → 25 high → 12 low → ...) pid 8
```

- 每个交互程序 spawn 后都拿到自己的键盘输入,ush 在期间未抢字符(`-EBUSY` 静默等待生效)
- 交互程序退出后 ush 立即恢复 `$` 提示符
- 同一程序连续 spawn 4 次(pid 2-6)均稳定(hello×4 回归测试)

## 文件清单

| 文件 | 修改 |
|------|------|
| `drivers/keyboard.c` | `kbd_owner` 全局 + `keyboard_acquire`/`keyboard_release`;`keyboard_read` 惰性获取、非 owner 返回 0 |
| `drivers/keyboard.h` | 声明 acquire/release |
| `kernel/syscall.c` | `sys_input` 对 DD_KEYBOARD 返回 `-EBUSY`;`sys_crtproc` 成功后让出键盘;`proc_exit_current` 释放键盘 |
| `kernel/syscall.h` | `EBUSY = 6` 错误码 |
| `kernel/programs/ush.c` | `-EBUSY` 静默重试(去掉刷屏打印) |
| `kernel/programs/{hello,echo,primes,fib,chars,calc,guess}.c` | 全面 syscall 化 + 补 `exit(0)` |
| `kernel/exec.c` | 删除 BSS 清零(根治头破坏 bug) |
| `build_64_boot.sh` | 删除用户程序的内核符号 `--defsym` |

## 已知限制

- **Zombie 不回收**:退出进程 PCB 槽位永久 ZOMBIE,PID 不复用(16 个进程后无法再 spawn,需重启)
- **无抢占式让出**:前台进程持有键盘期间,shell 只能空转等它退出(无 Ctrl+C / job control);教学 OS 的简化,后续可加"shell 特权抢占"或信号
- **-EBUSY 空转**:非 owner 进程的 input 是忙等重试,不阻塞、不耗大量 CPU(抢占式调度下可接受)
- **内存上限**:60MB/槽位,512MB 下最多约 8 个并发用户进程(pid 1-8)
- `keyboard_read` 驱动层的返回 0 语义依赖调用方忽略(echo 已处理;calc/guess 天然忽略非数字字符)
