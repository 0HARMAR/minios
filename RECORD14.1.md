# RECORD14.1 - 放弃键盘惰性所有权机制

## 背景

RECORD14 引入的惰性键盘所有权(`kbd_owner` + acquire/release + `-EBUSY`)在实际推演与使用中暴露出多个设计问题。它们不是实现 bug,而是**机制本身的构造错误**——补丁修不好,只能整体回退。本次 commit `8f8a5e3` 移除全部所有权代码,键盘回到共享环形缓冲、先到先得的原状(RECORD14 之前的行为)。

## 为什么放弃:设计问题清单

### 1. 两层检查点、语义分裂

同一件事(键盘被别人持有)有两套错误语义:syscall 层返回 `-EBUSY`,驱动层 `keyboard_read` 返回 0。驱动层的"返回 NUL"依赖调用方**恰好忽略 NUL** 才安全——这是脆弱的隐式契约,RECORD14 的已知限制里自己已经承认。一个机制需要两层防御、两种错误约定,说明它的位置放错了。

### 2. -EBUSY 是忙等协议,不是 wait

没有阻塞原语,非 owner 进程只能空转重试。前台程序(calc/guess)运行期间 ush 持续烧 CPU 轮询 `input`。这在抢占式调度下"可接受",但本质是把等待伪装成轮询——正确形态应当是"无权/无数据 -> 进程 sleep",而不是让所有相关进程都参与自旋。

### 3. crtproc 的隐式让出:policy 混进机制

`sys_crtproc` 成功后偷偷新增了"调用者让出键盘"的副作用。进程创建与终端归属是两件不相干的事,绑在一起后:

- 想边 spawn 子进程边继续读输入的程序(父进程交互场景)**静默丢键盘**,且无 API 拿回;
- `run primes` 这类不读键盘的 spawn 也强制 ush 让出再抢回,所有权在空闲态来回震荡。

spawn 的行为不再由 flags 控制,而是被内核里一条硬编码的 job-control 策略劫持。

### 4. 惰性获取存在 TOCTOU

`keyboard_acquire` 是非原子的 check-then-set。抢占式调度(时钟中断)下,两个进程可能都看到 `kbd_owner == 0` 并都"首次获取"成功——字符交错被抢,恰好是这个机制要防的事。要修就得加临界区/原子操作,复杂度继续上涨。

### 5. release 依赖退出路径完备,且无强制回收

"进程退出必须归还"是一条靠**每条退出路径都记得调用**维持的不变量(exit、PF 被杀、未来的 kill……)。漏掉任何一条,键盘被永久锁死,而系统没有特权强制回收入口(ush 无法夺回)。脆弱不变量 + 无恢复手段,是死锁面。

### 6. 概念错位(根因)

终端归属本质是**调度/策略**问题:Unix 用前台进程组 + controlling tty + SIGTTIN 解决,地基是"阻塞读 + wait queue"。在驱动里贴一个 pid 标签:

- 表达不了进程组(shell 代理一组子进程);
- 给不了 Ctrl+C / job control;
- 无法演进出真正的终端语义——是一个死胡同设计。

**结论:单独的 ownership 标签在错误的层解决错误的问题。正确路径是把 input 变成阻塞原语(sleep 直到"有数据且有权"),所有权概念留到 wait queue 就绪之后再设计。** 到那时本机制的所有场景(惰性获取/让出/归还)都会被更低的机制自然覆盖。

## 本次移除内容(commit 8f8a5e3)

| 文件 | 移除 |
|------|------|
| `drivers/keyboard.c` | `#include "process.h"`、`kbd_owner` 全局、`keyboard_acquire`/`keyboard_release`、`keyboard_read` 的 owner 检查 |
| `drivers/keyboard.h` | acquire/release 声明 |
| `kernel/syscall.c` | `sys_input` 的 DD_KEYBOARD/-EBUSY 拦截、`sys_crtproc` 成功后的隐式让出 |
| `kernel/syscall.h` | `EBUSY = 6` 错误码 |
| `.gitignore` | +`DESIGN_IP.md`、`.idea`(下一阶段:IP 协议栈设计稿) |

## 残留 Bug:回退不彻底,构建已断

`kernel/syscall.c` `proc_exit_current()` 中仍残留:

```c
    /* give the keyboard back to the shell (pid 0) */
    keyboard_release(current->pid);
```

`keyboard_release` 已随机制删除,全仓库无定义——链接必然 `undefined reference`,当前 HEAD **无法构建**。

**修复:删除 syscall.c:182 及其上一行注释**(连同 `keyboard.h` 里 syscall.c 已不再需要的引用一并检查),然后重跑下方验证。

## 保留的痕迹(无害,可留)

`ush.c:78`、`calc.c:47/75`、`echo.c:33`、`guess.c:74` 仍有 `if (input(1, &c, 1) < 0) continue;` 重试循环。移除所有权后 `input` 只在 EBADF/EFAULT 时返回负值(正常路径不触发),这些循环成为死代码。**建议保留**:它是未来 input 阻塞化(sleep/唤醒)时用户态重试逻辑的现成伏笔。

## 回退后的语义

- 键盘缓冲回到共享先到先得:任何进程的 `input(DD_KEYBOARD)` 都可消费字符,RECORD14 开头的"后台进程抢字符"问题**回归**。当前接受——单前台交互的使用模式下没有竞争者,风险已知。
- `run calc` 等交互程序行为不变:spawn 后独占物理键盘只是因为"没有别人在读",而非权限保证。
- RECORD14 的其余成果**全部保留**:用户程序全面 syscall 化、exec_init BSS 清零根治、`exit(0)` 收尾——这些与所有权无关,是独立的正确修复。

## 验证

1. 删除 syscall.c:182 残留调用后重新构建;
2. `-m 512` 下重跑 RECORD14 的全量 spawn 回归(hello/primes/fib/chars/calc/echo/guess,连续 spawn 4 次稳定性);
3. 确认 calc/guess 交互期间 ush 提示符行为(回退后 ush 与前台程序理论上可抢字符——单人使用下不应观察到)。

```bash
qemu-system-x86_64 -m 512 -hda long-mode/output/disk.img
```

## 下一步

`DESIGN_IP.md`(.gitignore 已登记)——转向 e1000 之上的 IP 层设计。键盘/终端的权限问题挂起,待 wait queue(阻塞读)落地后按第 6 条的正确形态重做。
