# RECORD15 — 补记 e1000 驱动，以及 ARP/IP 接收链路的打通

## 0. 为什么现在写这篇

e1000 驱动是在 commit `27690b7`（2026-08-09）落地的，当时只写了 `DESIGN_E1000.md`，没有按惯例留 RECORD。之后 IP 协议栈的设计（`DESIGN_IP.md`，08-14）和实现笔记（`IMPL_IP.md`，09-06）陆续补上，但代码一直躺在工作区没有提交。本篇把两段接起来：先补 e1000，再讲架在它上面的 ARP/IP，最后是这次把接收链路真正点亮的调试过程。

| 时间 | 事件 |
|---|---|
| 2026-08-09 | commit `27690b7`：e1000 驱动 + PCI 扫描 + raw frame syscall，打通帧级收发 |
| 2026-08-14 | `DESIGN_IP.md`：ARP / IPv4 / ICMP 设计 |
| 2026-09-06 | `IMPL_IP.md`：分批实现笔记 |
| 2026-11-14 | 接收侧调试（`IP_TEST_MODE`）、引导扇区修复、TAP 下宿主机 ping 验证 |

---

## 1. e1000 驱动补记

### 1.1 目标与边界

目标只有一个：**内核能收发以太网帧**。

这一层刻意不碰 TCP/IP，连用户态接口也停在帧级：

```c
#define SYS_NETSEND  5
#define SYS_NETRECV  6
#define SYS_NETMAC   7
```

`net.c` 因此可以手工拼一个 42 字节的 ARP 请求，用 `netsend` 发出去、`netrecv` 收回来，在不依赖任何协议栈的前提下验证"网卡 + DMA + 中断 + 环形队列"这条最底层的路。

### 1.2 五个卡点

| # | 现象 | 根因 | 解决 |
|---|---|---|---|
| 1 | 读写 e1000 寄存器全是 0 | QEMU 的 e1000 **I/O BAR 是空实现**（`e1000_io_read/write` 直接返回 0） | 只用 MMIO（BAR0），地址运行时从 PCI config 读，不硬编码 |
| 2 | 访问 BAR0 直接 page fault | MMIO 在 `0xFEB80000`，超出启动页表只映射的 1GB | `boot_64.S` 增加 `pd2/pd3/pd4` 映射 1GB–4GB；`paging.c` 每个进程 PDP[1..3] 共享这三张表 |
| 3 | 内核启动后反复复位 / 栈被清零 | NIC 静态缓冲（约 230KB）如果放进 `.bss`，会把 `_kernel_end` 顶到内核栈 `0x8F000-0x90000`，`exec_init` 启动清零时把正在用的栈清掉 | 新增 `.netbuf` NOLOAD 段，放在 `0x180000` |
| 4 | DMA 写进去的帧全是 `0xFF` | 缓冲落进了 `[0xA0000, 0xC0000)` 遗留 VGA 显存区，写入被 VGA 吞掉 | `.netbuf` 必须高于 1MB、低于用户区 `0x200000` |
| 5 | `-m 64` 下用户进程跑飞 | 16 个进程槽位共需约 960MB，pid 2 的栈页已超出 64MB，写入被静默丢弃 | 固定用 `-m 1G` |

这五条后来都写进了 `DESIGN_E1000.md` 第 2、11 节，属于"不看就会再踩一遍"的类型。

### 1.3 初始化序列

```text
1. PCI 扫描 8086:100E，取 BAR0 与 IRQ 11；PCI command = 0x07（MEM + I/O + bus master）
2. 软复位：CTRL.RST 置位并等待清零；再写 CTRL.SLU 置链路 up
3. 从 EEPROM 读 MAC：EERD 写 START|addr<<8，轮询 DONE(bit4)，数据在 [31:16]
4. MAC 写入 RAL0/RAH0（RAH0 bit31 = AV）
5. RX 环：32 个描述符 × 2048B；RDBAL/RDBAH/RDLEN；RDH=0，RDT=31
6. TX 环：16 个描述符 × 2048B；TDBAL/TDBAH/TDLEN；TDH=0，TDT=0
7. irq_register_handler(11, e1000_irq_handler)；pic_enable_irq(11)
8. RCTL = EN | UPE | BAM | SECRC；TCTL = EN | PSP；IMS = RXT0 | RXDMT0
```

几个位容易记错，代码里都留了注释：

- `TCTL.EN` 在 82540EM/QEMU 上是 **bit 1**，不是 bit 0；
- `RCTL.SECRC` 置位后网卡剥掉 CRC，`desc.length` 就是纯以太网帧长；
- `TCTL.PSP` 让网卡自动补齐短帧，ARP 的 42 字节帧不用软件 pad。

### 1.4 收与发

**接收**是中断驱动 + 软件环形队列：

```text
e1000 收包 → DMA 写 rx_bufs[i] + 写回描述符 status/length
    → IRQ 11
        → e1000_irq_handler：读 ICR（读即清）
            while (rx_ring[head].status & DD)
                memcpy(rx_pool[head], rx_bufs[i], len)
                rx_ring[i].status = 0
                RDT = i          // 必须先清 status 再还描述符
    → e1000_recv() 从 rx_pool 出队（非阻塞，空返回 0）
```

**发送**是纯轮询：

```text
e1000_send(frame, len)
    → 等 tx_ring[i].status & DD
    → memcpy(tx_bufs[i], frame, len)
    → cmd = EOP | IFCS | RS，status = 0
    → TDT = (i + 1) % 16        // TDT 是排他的，写 TDT == TDH 会被当成空环
```

TX 不开中断，靠 TDH/TDT 轮询；RX 只开 `RXT0`/`RXDMT0`，handler 里只做"搬帧 + 还描述符"，协议解析全部留给消费端。这条分工后来被 IP 层原样继承。

### 1.5 当时的验证

```bash
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 \
  -device e1000,netdev=net0 \
  -object filter-dump,id=f1,netdev=net0,file=/tmp/net.pcap \
  -m 1G
```

1. VGA 出现 `[e1000 ready] mac 52:54:00:12:34:56`；
2. `/tmp/net.pcap` 里能看到 `run net` 发出的 ARP 请求；
3. `run net` 打印 `[net] ARP reply! 52:55:0a:00:02:02`，说明 RX 闭环成立。

### 1.6 留给 IP 层的东西，和两个遗留

留下的基础：

- `.netbuf` 布局、RX/TX 环、`rx_pool`、IRQ 上下文"只搬数据"的约定；
- `e1000_send/e1000_recv/e1000_get_mac` 三个入口；
- 一组 raw frame syscall，让用户态可以绕过协议栈直接做链路实验。

两个遗留（这次读代码时发现）：

1. **`ICR_RXDMT0` 的位定义写错了**。`e1000.c` 里写的是 `(1 << 2)`，但 8254x 的 ICR bit2 是 **LSC（链路状态变化）**，`RXDMT0` 在 **bit4**。也就是说 `IMS` 实际打开的是 `RXT0 + LSC`，`RXDMT0` 从未启用。当前靠 `RDTR=0` 时 `RXT0` 每包中断掩盖了这个问题，链路 up/down 时会误入收包分支（无害但语义不对）。
2. 接收侧不检查 `rx_desc.errors`、不开组播（`RCTL.MPE`）、`rx_pool` 满了静默丢，也没有任何丢包计数。

---

## 2. 从帧到包：ARP/IP 协议栈

### 2.1 文件与分层

```text
long-mode/net/
├── bswap.h     be16/be32/get_be32/put_be32
├── ip.h  ip.c  IPv4 头、校验和、ip_send、ip_poll
└── arp.h arp.c ARP 缓存、request/reply、handle
```

`kernel.c` 在 `e1000_init()` 之后调用 `ip_init()`；`build_64_boot.sh` 增加 `-Inet` 并编译、链接 `ip.o`/`arp.o`。

分层关系很直接：

```text
应用 / 测试程序
    ↓（目前仍是 raw frame: netsend/netrecv）
ARP  ←  ip_poll  ←  e1000_recv  ←  rx_pool  ←  IRQ
IP   →  arp_resolve → e1000_send
```

### 2.2 地址与常量

| 项 | 值 |
|---|---|
| 本机 | `10.0.2.16/24`（`IP_ADDR`） |
| 掩码 | `255.255.255.0` |
| 网关 | `10.0.2.2`（slirp） |
| TTL | 64 |
| MTU / 最大载荷 | 1500 / 1480 |

这里有一个历史遗留：`IMPL_IP.md` 里曾把本机写成 `.15`，`net.c` 手工拼 ARP 时也写死 `10.0.2.15`，而内核 `ip.h` 最终是 `.16`。两套地址并存，slirp 因为会从帧里学习 IP↔MAC，暂时两者都能通；但 `hostfwd` 这类按配置地址投递的功能只认 `.15`。以后要么统一到 `.15`，要么把地址做成可配置。

### 2.3 发送：`ip_send`

```text
ip_send(proto, dst, payload, len)
  1. 校验 len ≤ 1480、dst != 0
  2. nexthop = 同网段 ? dst : IP_GW
  3. arp_resolve(nexthop, &dst_mac)
       命中 → 继续
       未命中 → 广播 ARP request，返回 IP_EBUSY
  4. 组 14 字节以太网头 + 20 字节 IP 头
  5. 算 IP 头校验和（只覆盖 20 字节头）
  6. 不足 60 字节补零
  7. e1000_send
```

设计上明确"**不做 pending 队列**"：ARP 没解析出来就返回 `IP_EBUSY`，由调用者自己 `ip_poll()` 后重试。好处是实现简单；代价是每个上层协议都得写一遍"重试 + 延时"的循环，而且每次重试都会重新广播一次 ARP。

其他已知边界：

- 目的地址是本机时不会走 loopback，而是去 ARP 自己，永远解析不到；
- `IP_BCAST` 定义了但没使用，`255.255.255.255` 会走"跨网段 → 网关 ARP"分支；
- 1514 字节的帧缓冲开在栈上，而用户进程的内核栈只有 4KB，挂到 syscall 上会很危险；
- 无分片、无 DF 位、无路由表、无源地址选择。

### 2.4 ARP

8 项静态缓存，`lookup/update` + 广播 `request` + `reply`：

| 收到 | 动作 |
|---|---|
| request，`tpa == IP_ADDR` | 学习 `spa/sha`，回 reply |
| reply | 学习 `spa/sha`，置 valid |
| 其他 | 只学习，不响应（不做代理 ARP） |

不足：没有老化/LRU（满了直接覆盖第 0 项）、不校验 op 只能是 1/2、reply 不校验 `tha`、只在 `ip_poll()` 被调用时才工作。ARP probe（`spa=0`）的行为是对的：`arp_update` 拒绝学习 0 地址，但仍会回 `spa=0` 的应答。

### 2.5 接收：当时只有 ARP

`ip_poll()` 的结构是：

```c
while ((n = e1000_recv(frame, sizeof(frame))) > 0) {
    if (n < 14) continue;
    et = frame[12]<<8 | frame[13];
    if (et == 0x0806) arp_handle(frame, n);
    /* 0x0800 在这里被丢掉 */
}
```

而 `ip_recv()` 是空桩，直接返回 0。

所以到这次调试之前，结论很明确：**接收侧只有 ARP 是活的**；IPv4/ICMP/UDP/TCP 的接收支持为零。`ip_poll()` 又只在 `ip_init()` 启动自检里被调用过一次，启动之后没有任何东西消费 `rx_pool`——e1000 中断照样把帧搬进池子，64 个槽满了之后开始静默丢包。

---

## 3. `IP_TEST_MODE`：把接收侧点亮

### 3.1 为什么不用时钟中断

最初的设想是"每次时钟中断调一次 `ip_poll()`"。分析后否掉了：

- timer handler 走中断门，IF=0，里面不能睡眠；
- `ip_poll()` 在栈上开 `frame[2048]`，而从用户态进来的中断用的是该进程 **4KB 内核栈**；
- `ip_init()` 期间 timer 已经在跑，会和启动循环里的 `ip_poll()` 重入；
- 串口一行约 40 字符，38400 下要 10ms，正好是一个 tick，在中断里逐包打印会把节拍吃穿。

最终选择：**测试模式不启动 `ush`，内核在 `kernel_main` 里死循环 `ip_poll()`**。它运行在 boot stack（`0x90000`）上、IF=1，既没有栈问题，也没有重入问题。

### 3.2 实现

`ip.h` 增加开关：

```c
#ifndef IP_TEST_MODE
#define IP_TEST_MODE 1
#endif
```

`kernel_main()`：

```c
#if IP_TEST_MODE
    ip_test_send_echo();          // 发一个 ICMP echo 到网关，制造入站流量
    for (;;)
        ip_poll();                // 纯忙等，死等，不设超时
#else
    ... spawn ush + idle loop ...
#endif
```

`ip_poll()` 增加 `0x0800` 分支，指向 `ip_handle_dbg()`：

```c
#if IP_TEST_MODE
else if (et == 0x0800) ip_handle_dbg(frame, n);
#endif
```

`ip_handle_dbg()` 只做校验和打印，不做任何协议处理：

```text
n-14 ≥ 20
version == 4，IHL ≥ 20，total_len ∈ [IHL, n-14]
IP 头校验和 == 0
MF/offset == 0（分片直接丢）
dst ∈ {IP_ADDR, IP_BCAST}
然后串口打印：[ip] src > dst proto N len M [icmp type/code]
```

### 3.3 为什么是串口

VGA 输出在这个场景有三个问题：

- 逐字符写显存 + `update_cursor` 的端口 I/O，比串口还慢；
- 和用户 shell 共用同一个 `vga_cursor_pos`，输出会交织；
- 一旦以后把 `ip_poll()` 挪进中断，VGA 打印会直接拉长中断。

串口代价也有：COM1 初始化成 38400 8N1，约 **260µs/字符**，一行 40 字符就是 10ms。所以只适合"少量包 + 调试"，真要持续抓包得把 divisor 从 3 改成 1（115200），或者只打统计。

### 3.4 第一次验证

在 slirp 下启动（`-netdev user`），串口（`-serial file:`）能看到：

```text
[ip] echo -> 10.0.2.2 sent
[ip] 10.0.2.2 > 10.0.2.16 proto 1 len 28 icmp 0/0
```

第一行是自己发出去的 echo，第二行是 slirp 回来的 echo reply。到这一步，`e1000_recv → ip_poll → ip_handle_dbg` 这条接收链路第一次被真正跑通。

---

## 4. 启动失败：131 个扇区读不进来

### 4.1 症状

新构建的镜像在 QEMU 里**根本进不了内核**：串口为空，`-display none` 下看不到任何输出。用 monitor 的 `info registers` 看到 CPU 停在：

```text
EIP=00007c3b  EAX=00000183  CS=0000  CR0=00000010
```

`0x7C3B` 正是 `boot_loader.S` 里 `disk_error` 后面的 `jmp $`。

### 4.2 根因

`boot_loader.S` 原来用一次 CHS `int 13h AH=02` 读入整个 payload：

```asm
movb $SECTORS, %al     # SECTORS = 131
...
int $0x13
jc disk_error
```

而构建脚本算出来的 `SECTORS` 是 `payload/512 + 1`，当前 payload 66264 字节，等于 **131**。BIOS 单次读取的上限是 127 个扇区，SeaBIOS 直接返回 `AH=01`（参数非法）——`EAX=0x0183` 里的 `AH=1`、`AL=0x83` 正好对上。

这个上限不是这次引入的，只是 payload 长到了 130 个扇区，第一次踩中。

### 4.3 修复

把 MBR 改成 **LBA 扩展读（`int 13h AH=42h`），一次读一个扇区**：

```asm
; 先检查扩展是否存在：AH=41h, BX=0x55AA
; DAP: size=0x10, count=1, offset, segment=0x07E0, LBA=1
read_loop:
    movb $0x42, %ah
    movw $dap, %si
    int  $0x13
    addw $0x200, dap_off
    jnc  .no_wrap
    addw $0x1000, dap_seg      ; offset 回绕时进一段
.no_wrap:
    addl $1, dap_lba
    decw remaining
    jnz  read_loop
```

一次一个扇区看起来很笨，但 131 次 BIOS 调用在启动阶段完全无感，而且顺带绕开了两个经典限制：单次 127 扇区上限、以及传输跨越 64KB 边界。

### 4.4 验证

修复后同一个镜像直接启动，串口输出第 3.4 节那两行。也就是说：**IP 调试代码本身没有问题，问题在引导扇区。**

---

## 5. TAP：让宿主机的 ping 进来

### 5.1 slirp 的限制

`-netdev user` 是用户态 NAT，只支持 `hostfwd` 的 TCP/UDP 转发。**入站 ICMP 没有通道**：宿主机 `ping 10.0.2.16` 根本到不了 guest，slirp 也不会替它转发。

所以"宿主机 ping guest"必须换二层通路。

### 5.2 配置

WSL2 里 `/dev/net/tun` 存在、`tun` 模块已加载，可以直接建 tap：

```bash
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip addr add 10.0.2.1/24 dev tap0
sudo ip addr add 10.0.2.2/24 dev tap0   # 临时：让现有 IP_GW 的启动 ARP 测试直接通过
sudo ip link set tap0 up

qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device e1000,netdev=net0 \
  -display none -monitor none -serial stdio -m 1G
```

另开一个终端：

```bash
ping -c 3 10.0.2.16
```

### 5.3 结果

```text
[ip] echo -> 10.0.2.2 sent
[ip] 10.0.2.2 > 10.0.2.16 proto 1 len 28 icmp 0/0
[ip] 10.0.2.1 > 10.0.2.16 proto 1 len 84 icmp 8/0
[ip] 10.0.2.1 > 10.0.2.16 proto 1 len 84 icmp 8/0
[ip] 10.0.2.1 > 10.0.2.16 proto 1 len 84 icmp 8/0
```

前两行是启动自测；后三行是宿主机 `ping` 发来的 echo request，guest **只记录、不应答**，所以宿主机侧是 100% packet loss——这正是预期。

宿主机能发出 ICMP 的前提是 ARP 先通：guest 的 `arp_handle` 回了 ARP reply，宿主机才拿到 MAC。也就是说"不应答 ICMP"和"不应答 ARP"是两回事，后者必须保留。

### 5.4 清理

```bash
sudo ip link set tap0 down
sudo ip tuntap del dev tap0 mode tap
```

---

## 6. 当前状态

### 6.1 工作区（未提交）

| 文件 | 内容 |
|---|---|
| `long-mode/net/`（5 个新文件，456 行） | `arp.c/h`、`ip.c/h`、`bswap.h` |
| `long-mode/kernel/kernel.c` | `ip_init()` + `IP_TEST_MODE` 测试分支 |
| `long-mode/build_64_boot.sh` | 编译/链接 `net/*.o` |
| `long-mode/boot_loader.S` | LBA 逐扇区读取，修复 >127 扇区无法启动 |
| `IMPL_IP.md` | IP 实现笔记（未跟踪） |

当前工作区停在**手动验证过的那一版**：启动时发一次 ICMP echo、打印所有合法 IP 包、`IP_GW = 10.0.2.2`。更精简的一版（`IP_TEST_MODE` 下网关改成 `10.0.2.1`、去掉启动 echo、只打印 ICMP echo request）已经写过，但为了先提交一个可复现的基线，暂时还原掉了，等基线提交后再落地。

### 6.2 已验证

- e1000：初始化、MAC、TX/RX、IRQ、pcap；
- ARP：request/reply、缓存学习、启动时解析网关；
- IP 发送：`ip_send` 发出合法 IPv4 头（slirp 能回）；
- IP 接收：`0x0800` 分发、头校验、串口打印；
- 引导：LBA 读取，131 扇区 payload 可启动；
- TAP：宿主机 `ping 10.0.2.16` 的 ICMP 请求能被 guest 收到并打印。

### 6.3 已知问题

1. **正常模式没有收包泵**。`IP_TEST_MODE=0` 时 `ip_poll()` 仍只在启动自检里调用；`ush` 在 `input()` 里自旋，pid 0 的 idle loop 永远轮不到。要么把 `ip_poll()` 挂到键盘等待循环，要么等 input 阻塞化之后挂 idle。
2. **没有 ICMP 应答**。收到 echo request 只打印，不回 reply；也没有 ICMP 错误处理。
3. **没有 UDP/TCP，也没有 IP 层用户态接口**。`SYS_IPSEND/IPRECV/IPINFO` 还没做。
4. **ARP 没有老化、没有 pending 队列**。MAC 变化不会更新；ARP 未解析时包直接丢，重试全靠上层。
5. **`ICR_RXDMT0` 位定义错误**（bit2 是 LSC，bit4 才是 RXDMT0），见 1.6。
6. **`.netbuf` 正在被 payload 逼近**。测试模式 payload 已到 `0x180D8` 左右，而 `.netbuf` 从 `0x180000` 开始；引导加载会压到 `rx_ring`，目前靠 `e1000_init` 重新初始化描述符兜住。继续加代码前要重新算 `0x180000` 这个假设。
7. **栈与截断**。`ip_send` 的 1514 字节帧、`ip_poll` 的 2048 字节帧都在栈上；`e1000_recv` 在 `maxlen` 不足时静默截断，对 IP 来说等于制造坏包。
8. **地址不统一**。`ip.h` 是 `10.0.2.16`，`net.c` 手工帧和 `IMPL_IP.md` 是 `10.0.2.15`。

### 6.4 下一步

1. 提交当前验证过的基线（ARP/IP + 接收调试 + 引导修复）；
2. 恢复精简版调试：`IP_TEST_MODE` 下 `IP_GW=10.0.2.1`、去掉启动 echo、只打印 ICMP echo request；
3. 正常模式给 `ip_poll()` 找挂载点（键盘等待循环或 input 阻塞化后的 idle loop）；
4. 视需要加 ICMP echo reply；
5. 之后才是 UDP socket、TCP。

---

## 7. 命令速查

```bash
# 构建
cd long-mode && bash build_64_boot.sh

# slirp 下自测（启动 echo + reply）
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -display none -monitor none -serial stdio -m 1G

# TAP 下让宿主机 ping 进来
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip addr add 10.0.2.1/24 dev tap0
sudo ip addr add 10.0.2.2/24 dev tap0
sudo ip link set tap0 up
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device e1000,netdev=net0 \
  -display none -monitor none -serial stdio -m 1G
# 另一个终端：ping -c 3 10.0.2.16

# 抓包
qemu-system-x86_64 ... -object filter-dump,id=f1,netdev=net0,file=/tmp/net.pcap
```
