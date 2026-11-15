# RECORD16 — TCP 服务端三次握手

## 背景与范围

RECORD15.1 之后，ICMP 已经能回应外部 ping。下一步是 TCP，但完整 TCP 的状态机、缓冲、重传、阻塞接口一次做完不现实。这一轮只做**服务端被动打开的三次握手**，作为 P0 的第一个里程碑。

范围是讨论后定死的：

- 只做服务端：`LISTEN → SYN_RCVD → ESTABLISHED`；
- 不做客户端、数据收发、FIN/TIME_WAIT、accept 队列；
- 只处理正常三次握手：不做重复 SYN 重发、不做无监听端口 RST、不做非法 ACK 的 RST；
- 验证方式：TAP + 宿主机 `nc -vz`；
- MSS=1460，窗口字段固定 4096；
- 日志记录 `SYN`、`SYN-ACK`、`ACK -> ESTABLISHED`，带 seq/ack/flags；
- LISTEN TCB 常驻，收到 SYN 分配子 TCB；池 4 项；
- 握手完成后 ESTABLISHED 保留，测完重启 QEMU。

这一版的握手是**单发**的：SYN 或 SYN-ACK 丢了就失败，SYN 重传留给后续的 RTO 阶段。

## 设计

### 1. 依赖与入口

| 依赖 | 状态 |
|---|---|
| 收包泵 | 测试模式 `for (;;) ip_poll();` 已具备 |
| `ip_handle` 分发 proto=6 | 本次新增 |
| IP 头校验/分片丢弃/目的过滤 | 已有 |
| 源 MAC 学习 | 已有（`ip_learn_src`），SYN-ACK 不会因 ARP 未命中而 EBUSY |
| TCP 伪首部校验和 | 本次新增 |
| TCB 与序号 | 本次新增 |

`ip_handle()` 增加：

```c
else if (h->proto == IP_PROTO_TCP)
    tcp_input((uint8_t *)h, ihl, total);
```

`kernel_main()` 的测试分支在进入死循环前调用：

```c
tcp_listen(1234);
serial_write("[tcp] listen 1234\n", ...);
```

### 2. TCB 与监听模型

```c
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
};

struct tcp_tcb {
    enum tcp_state state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    uint32_t iss, snd_una, snd_nxt;
    uint16_t snd_wnd, mss;

    uint32_t irs, rcv_nxt;
    uint16_t rcv_wnd;
};
```

- 池 4 项：1 个 LISTEN + 最多 3 个连接；
- LISTEN TCB 不参与 4 元组匹配，只在 `dport` 命中时作为回落；
- 收到 SYN 时从池里分配子 TCB，LISTEN 保持不变；
- 子 TCB 记录对端 IP/端口、`irs`、`rcv_nxt = seq+1`、`iss`、`snd_una = iss`、`snd_nxt = iss+1`、对端窗口/MSS；
- 没有 accept 队列：子 TCB 进入 ESTABLISHED 后就停在那里。

### 3. 序号与 ISN

```c
static uint32_t tcp_isn(void)
{
    uint32_t v = (uint32_t)timer_get_ticks() * 64000u
               + (tcp_isn_counter++ << 12)
               + 0x12345u;
    return v ? v : 1u;
}
```

- SYN 占 1 个序号：`rcv_nxt = syn.seq + 1`，`snd_nxt = iss + 1`；
- ACK 必须满足 `ack == snd_nxt && seq == rcv_nxt` 才进入 ESTABLISHED；
- `nc -vz` 的 ACK（可能带 FIN）满足该条件，先处理 ACK，FIN 忽略。

### 4. 校验和（伪首部）

TCP 校验和覆盖：

```text
伪首部：src(4) + dst(4) + 0 + proto=6 + tcp_len(2)
TCP 头 + 选项 + 数据
```

实现上没有复用 `csum16` 的内存序技巧，而是写了一个按**网络字节序**累加的 `csum_partial()`：

```c
sum += ((uint32_t)p[0] << 8) | p[1];
```

奇数尾字节按高字节处理。`tcp_checksum()` 先累加 12 字节伪首部，再累加 TCP 段，折叠取反。

- 接收：整段（含 checksum 字段）算出来必须为 0；
- 发送：checksum 字段先清零，算完按大端写回 `seg[16]/seg[17]`。

`nc` 能接受 SYN-ACK，说明 Linux 侧校验通过。

### 5. MSS 选项

- SYN-ACK 固定带 MSS=1460（4 字节，data offset=6，头长 24）；
- 接收方向解析对端 MSS：从 offset 20 扫到 data offset，处理 kind=0（结束）、kind=1（NOP）、kind=2 len=4（MSS），其他选项按长度跳过；
- 默认 536，夹在 `[64, 1460]`；
- 子 TCB 保存对端 MSS，供以后发送数据用。

### 6. 状态机

```text
LISTEN
  ← SYN(seq=client_isn, flags=0x02)
  → SYN-ACK(seq=iss, ack=client_isn+1, flags=0x12, win=4096, MSS=1460)
SYN_RCVD
  ← ACK(seq=client_isn+1, ack=iss+1, flags=0x10)
ESTABLISHED
```

- LISTEN 收到非 SYN、或 SYN 带 ACK/RST：忽略；
- SYN_RCVD 收到 RST：释放 TCB（最小的异常路径）；
- SYN_RCVD 收到 ACK 但 seq/ack 不匹配：忽略，不回 RST（本阶段范围）；
- ESTABLISHED 之后的数据/FIN：忽略。

### 7. 日志

只在 `IP_TEST_MODE` 下输出：

```text
[tcp] SYN 10.0.2.1:49052 -> :1234 seq=1231578761 flags=0x02
[tcp] SYN-ACK seq=8394565 ack=1231578762 flags=0x12
[tcp] ACK -> ESTABLISHED seq=1231578762 ack=8394566
```

`flags` 用两位十六进制；`SYN-ACK` 发送失败会在行尾加 `send-fail`。

## 实现

| 文件 | 改动 |
|---|---|
| `net/tcp.h`（新增，50 行） | 状态、flags、TCB、`tcp_listen/tcp_input` |
| `net/tcp.c`（新增，299 行） | TCB 池、ISN、伪首部校验和、MSS 解析、SYN/SYN-ACK/ACK 状态机、日志 |
| `net/ip.c` | `proto == 6 → tcp_input()` |
| `kernel/kernel.c` | 测试模式 `tcp_listen(1234)` + `[tcp] listen 1234` |
| `build_64_boot.sh` | 编译/链接 `tcp.o` |

`IP_TEST_MODE=0` 也编译通过；TCP 输入路径始终存在，只是没有收包泵和监听初始化。

## 验证

### 环境

sudo 需要密码，非交互环境里用 user + network namespace 拿 CAP_NET_ADMIN（与 ICMP 的 TAP 验证相同）：

```bash
unshare -Urn bash -c '
  ip link set lo up
  ip tuntap add dev tap0 mode tap
  ip addr add 10.0.2.1/24 dev tap0
  ip addr add 10.0.2.2/24 dev tap0
  ip link set tap0 up
  qemu-system-x86_64 -drive format=raw,file=/tmp/tcp_test.img \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device e1000,netdev=net0 \
    -display none -monitor none -serial file:/tmp/tcp_hs.log -m 1G
  nc -4 -vz -w 2 10.0.2.16 1234
'
```

`10.0.2.2` 别名是给 `ip_init()` 的网关 ARP 测试用的，让启动不用等 200 轮。

### 结果

连续两次 `nc -vz` 都成功：

```text
Connection to 10.0.2.16 1234 port [tcp/*] succeeded!
Connection to 10.0.2.16 1234 port [tcp/*] succeeded!
```

guest 串口：

```text
[tcp] listen 1234
[icmp] ready
[tcp] SYN 10.0.2.1:49052 -> :1234 seq=1231578761 flags=0x02
[tcp] SYN-ACK seq=8394565 ack=1231578762 flags=0x12
[tcp] ACK -> ESTABLISHED seq=1231578762 ack=8394566
[tcp] SYN 10.0.2.1:49068 -> :1234 seq=3703846589 flags=0x02
[tcp] SYN-ACK seq=14990661 ack=3703846590 flags=0x12
[tcp] ACK -> ESTABLISHED seq=3703846590 ack=14990662
```

两次源端口不同，说明 LISTEN TCB 保留、子 TCB 分配正常。`nc` 能成功也验证了：

- SYN-ACK 的 IP/TCP 校验和正确；
- seq/ack 与 Linux 的期望一致；
- MSS 选项格式正确；
- 窗口字段被接受。

## 已知边界

1. **单发握手**：没有 SYN/SYN-ACK 重传，丢包即失败；这是“状态机跑通”而不是“可靠握手”。
2. **没有数据路径**：ESTABLISHED 后的数据段直接忽略；`nc -vz` 只连接不传数据，所以能通过。
3. **没有 FIN/TIME_WAIT**：`nc -vz` 连上后立即关闭，guest 忽略 FIN，TCB 停在 ESTABLISHED。
4. **没有 accept 队列**：LISTEN 与连接 TCB 分离，但没有“已完成连接”队列供应用 accept。
5. **连接数有限**：池 4 项，1 个 LISTEN + 3 个连接；测 3 次后需要重启 QEMU。
6. **没有 RST 策略**：无监听端口、非法 ACK 都静默丢弃；SYN_RCVD 收到 RST 只做释放。
7. **窗口是固定值**：4096，没有接收缓冲，也没有窗口更新；数据阶段必须重做。
8. **正常模式没有收包泵**：只有 `IP_TEST_MODE=1` 的测试镜像会握手。
9. **奇数长度校验和未覆盖**：握手段都是偶数长度；`csum_partial` 的奇数尾字节逻辑与 ICMP 的已验证实现一致，但 TCP 数据阶段还要再测。

## 下一步

```text
数据 + 累计 ACK + 发送/接收环 + rwnd
  → RTO 重传 + 发送缓冲（真正可靠）
  → FIN/TIME_WAIT/RST
  → accept 队列 / socket API
```

## 提交

```text
实现TCP服务端握手
```

包含：`net/tcp.c`、`net/tcp.h`、`net/ip.c`、`kernel/kernel.c`、`build_64_boot.sh`、本文件。
