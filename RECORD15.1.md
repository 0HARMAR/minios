# RECORD15.1 — ICMP echo 应答（服务端）

## 背景

RECORD15 收尾时，接收侧只做到“校验 + 串口打印”：`ip_poll()` 在 `IP_TEST_MODE` 下把所有合法 IP 包打一行，启动时还会主动发一个 ICMP echo；没有任何协议应答能力。

这一步想把 ICMP 做成“能在外面真正 ping 通”。讨论后把范围砍到最小可用：

- **只做服务端**：收到 echo request 回 echo reply；
- 不做客户端、不做错误报文（dest unreachable / time exceeded）、不做分片重组；
- 只在 `IP_TEST_MODE` 下跑（死循环收包，不启动 `ush`）；
- 只回单播，全局令牌桶限速；
- 从收到的 IP 帧学习源 MAC，避免回包时 ARP 未命中；
- 串口只打 ICMP 请求/应答，不再打印所有 IP 包。

## 设计

### 1. 接收入口：`ip_handle`

原来 `0x0800` 只有一个调试分支 `ip_handle_dbg`，现在换成生产版入口：

```text
ip_poll
  └─ 0x0800 → ip_handle
        ├─ n-14 ≥ 20
        ├─ version == 4、IHL ≥ 20、total_len ∈ [IHL, n-14]
        ├─ IP 头校验和 == 0
        ├─ 分片（MF | offset）→ 丢弃
        ├─ dst ∈ {本机, 255.255.255.255}
        ├─ 同网段源地址 → arp_update(源 IP, 帧头源 MAC)
        └─ proto == 1 → icmp_input(IP 头, IHL, total_len)
```

调试打印全部移除；非 ICMP 协议目前静默丢弃，给以后的 UDP/TCP 留分发点。

### 2. 源 MAC 学习

回 echo reply 要经过 `ip_send → arp_resolve(源 IP)`。如果对端不在 ARP 缓存里，`ip_send` 返回 `IP_EBUSY`，reply 直接丢。

在 `ip_handle` 里顺手学习：

- 源 IP 必须与本机同网段（跨网段时帧头源 MAC 是网关的，不能拿来当对端 MAC）；
- 源 IP 不能是 0 / 本机 / 广播 / 组播；
- 以太网源 MAC 不能是组播/广播位，也不能全 0；
- 满足则 `arp_update(src, frame + 6)`。

这样对方先 ARP 再 ping 的常规路径、以及“直接注入 IP 帧”的测试路径，都能正常回包。

### 3. echo reply

`icmp_input()` 只处理 `type=8, code=0`：

1. `ICMP 长度 ≥ 8`；
2. `csum16(icmp, icmp_len) == 0`（覆盖整个 ICMP 消息，无伪首部）；
3. 目的地址必须**恰好等于本机**（广播目的不回）；
4. 源地址不能是 0 / 本机 / 广播 / 组播；
5. 令牌桶检查；
6. 原地改写：`type=0, code=0`，checksum 清零后重算；
7. `ip_send(IP_PROTO_ICMP, src, icmp, icmp_len)`。

IP 头由 `ip_send` 重新构造：源地址 `IP_ADDR`、TTL 64、新 id、重算头校验和；ICMP 的 id/seq/数据原样回显。

原地改写用的是 `ip_poll` 的 `frame[2048]`，`ip_send` 会把 payload 再拷进自己的发送帧，不需要额外的 reply 缓冲。

### 4. 限速

全局令牌桶：

- 容量 10，初始 10；
- 每 10 个 tick（100ms）补 1 个 token，即 10/s；
- 取不到 token 时 `icmp_drop_rate++`，静默丢弃；
- 用 `timer_get_ticks()` 做时间基准，长时间无包时直接补满，避免循环。

计数器：`icmp_req_count`、`icmp_reply_count`、`icmp_drop_rate`、`icmp_bad`，目前只累加，不打印。

### 5. 日志

只在 `IP_TEST_MODE` 下输出，格式固定：

```text
[icmp] req 10.0.2.1 > 10.0.2.16 seq=1 len=84
[icmp] reply > 10.0.2.1 seq=1 len=84
```

`len` 是 IP 总长度（Linux ping 默认 56 字节载荷 → 20 + 8 + 56 = 84）。请求被限速、校验失败、广播、分片等都不打印。

### 6. 测试模式

`kernel_main()` 的测试分支变成：

```c
serial_write("[icmp] ready\n", sizeof("[icmp] ready\n") - 1);
for (;;)
    ip_poll();
```

原来的 `ip_test_send_echo()`（启动时主动 ping 网关）删除。`ip_init()` 里的网关 ARP 测试保留，TAP 场景下需要 tap0 上有 `10.0.2.2/24` 别名才能快速通过。

## 实现

| 文件 | 改动 |
|---|---|
| `long-mode/net/icmp.c` | 新增：校验和、echo reply、令牌桶、计数器、日志（137 行） |
| `long-mode/net/icmp.h` | 新增：`icmp_input()` 声明 |
| `long-mode/net/ip.c` | `ip_handle_dbg` → `ip_handle`；源 MAC 学习；按 proto 分发；删除启动 echo 和全量 IP 打印 |
| `long-mode/net/ip.h` | 删除 `ip_test_send_echo()`，更新测试模式注释 |
| `long-mode/kernel/kernel.c` | 测试模式打 `[icmp] ready`，删掉启动 echo |
| `long-mode/build_64_boot.sh` | 编译/链接 `icmp.o` |

## 验证

### A. 无 root 的原始帧注入

用 QEMU 的 socket netdev，不依赖 TAP/root：

```bash
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev socket,id=net0,udp=127.0.0.1:1234,localaddr=127.0.0.1:1235 \
  -device e1000,netdev=net0 \
  -display none -monitor none -serial file:/tmp/icmp.log -m 1G
```

Python 侧构造以太网 + IP + ICMP 帧，从 `127.0.0.1:1234` 发到 QEMU 的 `1235`，再收 reply。

| 用例 | 结果 |
|---|---|
| 3 个合法单播 echo | 3 个 reply，type=0、id/seq 一致、checksum 正确、payload 原样 |
| 目的 255.255.255.255 | 不回 |
| ICMP 校验和错误 | 不回 |
| MF 分片 | 不回 |
| IP 头校验和错误 | 不回 |
| 源地址 0.0.0.0 | 不回 |
| 20 个请求突发 | 只回 10 个（seq 1–10） |
| payload 0 / 1 / 33 字节 | 全部正确回显，奇数长度 checksum 通过 |

### B. TAP ping

`sudo` 需要密码，非交互环境里用 user + network namespace 拿 CAP_NET_ADMIN（数据路径与宿主 netns 完全一致，只是 tap0 在隔离命名空间里，测完自动销毁）：

```bash
unshare -Urn bash -c '
  ip link set lo up
  ip tuntap add dev tap0 mode tap
  ip addr add 10.0.2.1/24 dev tap0
  ip addr add 10.0.2.2/24 dev tap0
  ip link set tap0 up
  qemu-system-x86_64 -drive format=raw,file=output/disk.img \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device e1000,netdev=net0 \
    -display none -monitor none -serial file:/tmp/tap.log -m 1G
  ping -c 4 10.0.2.16
'
```

结果：

```text
64 bytes from 10.0.2.16: icmp_seq=1 ttl=64 time=2.56 ms
64 bytes from 10.0.2.16: icmp_seq=2 ttl=64 time=0.606 ms
64 bytes from 10.0.2.16: icmp_seq=3 ttl=64 time=0.714 ms
64 bytes from 10.0.2.16: icmp_seq=4 ttl=64 time=0.803 ms
4 packets transmitted, 4 received, 0% packet loss
```

guest 串口：

```text
[icmp] ready
[icmp] req 10.0.2.1 > 10.0.2.16 seq=1 len=84
[icmp] reply > 10.0.2.1 seq=1 len=84
...（seq 2/3/4 各一对）
```

`ttl=64`、`len=84` 都与实现一致。

## 已知边界

1. **没有客户端**。内核不会主动 ping 别人，也没有 RTT/超时统计。
2. **不解析错误报文**。收到 dest unreachable / time exceeded 只会静默丢弃；将来有 UDP/TCP 后才能映射成连接错误。
3. **不重组、不分片**。带 MF/offset 的 echo request 直接丢；reply 也不会超过 MTU。
4. **正常模式没有收包泵**。`IP_TEST_MODE=0` 时 `ip_poll()` 仍只在启动自检里调用；`ush` 在 `input()` 里自旋，pid 0 的 idle loop 轮不到。要让正常模式回 ping，得把 `ip_poll()` 挂到键盘等待循环，或等 input 阻塞化后挂 idle。
5. **限速与串口日志互相影响**。38400 下每行约 10ms，一组 req/reply 约 20ms；突发较大时限速会被日志拖慢（测试里 20 个包仍然只回了 10 个）。
6. **源 MAC 学习只对同网段生效**。跨网段 reply 走网关，依赖 `ip_init()` 已解析的网关 ARP。
7. **广播目的不回**。`ip_handle` 接受 `IP_BCAST` 以便将来别的协议使用，但 ICMP 只回 `dst == IP_ADDR`。
8. **没有 IP 选项回显**。IHL > 5 的请求能解析并回复，但 reply 不携带选项。

## 提交

```text
实现 ICMP echo 应答与接收分发
```

包含：`net/icmp.c`、`net/icmp.h`、`net/ip.c`、`net/ip.h`、`kernel/kernel.c`、`build_64_boot.sh`、本文件。
