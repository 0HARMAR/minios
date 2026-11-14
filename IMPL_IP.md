# IP 协议栈实现笔记（对应 DESIGN_IP.md，分批敲）

## Phase 1 / 第 1 批：两个头文件（纯声明，无逻辑）

先新建目录 `long-mode/net/`，放两个头文件。

### 新建 `long-mode/net/bswap.h`

```c
#ifndef BSWAP_H
#define BSWAP_H

#include <stdint.h>

/* host <-> network byte order。
 * be16/be32 是自反的：be16(be16(x)) == x，
 * 所以同一个函数既当 hton 也当 ntoh。
 * 全栈约定：IP 地址在 C 变量里存主机序数值（IP4(10,0,2,15)），
 * 只在读写 struct ip_hdr 字段时才转换。 */
static inline uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | (v >> 24);
}

#endif
```

### 新建 `long-mode/net/ip.h`

```c
#ifndef IP_H
#define IP_H

#include <stdint.h>

/* ---- 配置：编译期常量，对齐 QEMU slirp 默认 ---- */
#define IP4(a,b,c,d) (((uint32_t)(a)<<24) | ((uint32_t)(b)<<16) | \
                      ((uint32_t)(c)<<8)   |  (uint32_t)(d))
#define IP_ADDR    IP4(10, 0, 2, 15)   /* 本机 */
#define IP_MASK    IP4(255,255,255,0)
#define IP_GW      IP4(10, 0, 2, 2)    /* slirp 网关 */
#define IP_BCAST   0xFFFFFFFFu         /* 255.255.255.255 */

/* ---- 协议常量 ---- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17
#define IP_TTL        64
#define IP_MAX_PAYLOAD 1480    /* 1500 MTU - 20 头 */

/* ---- 错误码（负值返回；数值对齐 syscall.h 的 errno）---- */
#define IP_EINVAL (-5)
#define IP_EBUSY  (-6)         /* ARP 未解析，上层延时重试 */

/* ---- IPv4 头（20 字节无选项；线上大端）----
 * 多字节字段读出来是网络序数值，使用前必须 be16/be32。 */
struct ip_hdr {
    uint8_t  ver_ihl;    /* 0x45 */
    uint8_t  tos;
    uint16_t total_len;  /* 头 + 载荷 */
    uint16_t id;         /* 发送时全局自增 */
    uint16_t frag;       /* 0 = 不分片 */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;   /* 只覆盖 20 字节头 */
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

/* ---- 接口 ---- */
uint16_t csum16(const void *data, uint32_t len);  /* RFC1071 反码和 */
void     ip_init(void);
int32_t  ip_send(uint8_t proto, uint32_t dst, const void *payload, uint16_t len);
void     ip_poll(void);
int32_t  ip_recv(uint8_t *out, uint16_t maxlen);  /* 0 = 无包 */

#endif
```

### 本批说明

- 敲完这批**还编译不了也不需要编译**——纯头文件，没有任何 .c 引用它们。
- `IP_EINVAL/IP_EBUSY` 不 include syscall.h，让依赖方向保持 `syscall.c -> ip.h`。
- `struct ip_hdr` 里 `total_len` 等多字节字段读出来是**网络序**，比较前记得 be16/be32（这是 Phase 3 收包最容易踩的坑）。

### 下一批

`net/ip.c` 的第一部分：csum16 + print_ip。

---

## Phase 1 / 第 2 批：`net/ip.c` 上半部分（csum16 + print_ip）

新建 `long-mode/net/ip.c`，本批只写文件头 + 两个函数。

### 新建 `long-mode/net/ip.c`

```c
/* IPv4 layer: 封装/解封/校验和/接收分发（DESIGN_IP.md）。
 * Phase 1：csum16 + print_ip + ip_init 自检桩。
 * Phase 2 填 ip_send，Phase 3 填 ip_poll/ip_recv。 */
#include "ip.h"
#include "bswap.h"
#include "lib.h"     /* utoa */
#include "vga.h"     /* print_string / VGA_BUFFER */

extern uint32_t vga_cursor_pos;   /* kernel.c 定义，与 e1000.c 同款引用 */

/* ---- 校验和（RFC 1071 反码和）----
 * 16 位字求和 -> 奇数尾字节 -> 进位回卷 -> 取反。
 * 字节序性质（重要，坑注见第 3 批）：小端机按本机字累加大端缓冲，
 * 返回值恒为"网络序校验和"的 be16。推论：写回 struct 字段直接赋值
 * （勿 be16）；当数值比较/打印前先 be16。
 * IP 头校验和只覆盖 20 字节头；ICMP 校验和覆盖头+数据（无伪首部）。 */
uint16_t csum16(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;               /* 奇数尾字节 */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16); /* 回卷 */
    return (uint16_t)~sum;
}

/* ---- 点分十进制打印（10.0.2.15 样式）----
 * 后续 ARP 调试打印也复用。tmp 最大放 "255\0" = 4 字节。 */
static void print_ip(unsigned char *vga, uint32_t ip) {
    char tmp[4];
    for (int i = 3; i >= 0; i--) {          /* 最高字节在前 */
        utoa((uint32_t)((ip >> (i * 8)) & 0xFF), tmp);
        print_string(vga, tmp, &vga_cursor_pos);
        if (i) print_string(vga, ".", &vga_cursor_pos);
    }
}
```

### 本批说明

- `csum16` 与 DESIGN_IP.md §3.2 的唯一差别：去掉 `static`（声明在 ip.h），
  因为 Phase 3 的 icmp.c 也要用它。
- **注意坑**：`~sum` 在 C 里是 32 位取反，返回前必须 `(uint16_t)` 截断，
  否则高 16 位是 1，比较 `== 0xB861` 永远失败。
- `print_ip` 的 `tmp[4]`：`utoa` 最多写 "255" + '\0' = 4 字节，刚好塞满，别缩成 3。
- 编译还没接进 build 脚本，本批敲完 `ip.c` 只有半截，
  **先不编译**（ip_init/ip_send 等还没定义，链接不了）。

### 下一批

`net/ip.c` 下半部分：ip_init（自检向量）+ 三个桩函数，之后接进
kernel.c 和 build_64_boot.sh，首次编译验证。

---

## Phase 1 / 第 3 批：`ip_init` + 桩函数 + 接线（Phase 1 收尾，可编译）

本批内容：`ip.c` 补完（追加到第 2 批代码后面）+ 修改 kernel.c 和
build_64_boot.sh。做完后**首次编译链接通过，QEMU 出现验证点 1**。

### `net/ip.c` 追加（接在第 2 批后面）

```c
/* ---- RFC 1071 例向量：真实 IP 头 192.168.0.1 -> 192.168.0.199 ----
 * 校验和字段(偏移 10-11)置 0；网络序期望值 0xB861（比较前先 be16，见坑注）。
 * 自检防端序 bug：若 be16/累加方向有错，这里第一个暴露。 */
static const uint8_t ip_csum_test[20] = {
    0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
    0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
    0xC0, 0xA8, 0x00, 0xC7,
};

/* ip 发送 id 自增计数器（抓包对照用，DESIGN_IP §3.1） */
static uint16_t ip_id_counter;

void ip_init(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;

    print_string(vga, "[ip] ", &vga_cursor_pos);
    print_ip(vga, IP_ADDR);
    print_string(vga, "/24 gw ", &vga_cursor_pos);   /* 24 对应 IP_MASK */
    print_ip(vga, IP_GW);

    /* 小端机上 csum16 返回网络序值的 be16（本例 = 0x61B8），先 be16 还原 */
    if (be16(csum16(ip_csum_test, 20)) == 0xB861)
        print_string(vga, " csum ok", &vga_cursor_pos);
    else
        print_string(vga, " csum BAD", &vga_cursor_pos);
    print_newline(vga, &vga_cursor_pos);
    update_cursor(vga_cursor_pos);

    ip_id_counter = 0;
    /* Phase 3 在这里显式清零 ip_rx 队列指针（.netbuf 是 NOLOAD 段，
     * exec_init 不清零它，静态初始化值也不可靠，必须运行时清） */
}

/* ---- 以下三个是 Phase 2/3 的桩 ---- */
int32_t ip_send(uint8_t proto, uint32_t dst,
                const void *payload, uint16_t len) {
    (void)proto; (void)dst; (void)payload; (void)len;
    return IP_EINVAL;                 /* Phase 2 实现 */
}

void ip_poll(void) {
    /* Phase 3 实现：消费 e1000 收包队列并分发 */
}

int32_t ip_recv(uint8_t *out, uint16_t maxlen) {
    (void)out; (void)maxlen;
    return 0;                         /* 0 = 无包 */
}
```

#### 坑：校验和与字节序（第 3 批实测踩到，改完比较条件再重跑）

`csum16(x) == 0xB861` 在 x86 上**永远为假**。把同一数组按两种字序
求和（32 位累加完再回卷、取反）：

```
网络序字：4500 0073 0000 4000 4011 0000 C0A8 0001 C0A8 00C7
  和 0x2479C -> 回卷 0x479E -> 取反 = 0xB861   ← RFC 期望值
本机字(x86 实际算出)：0045 7300 0000 0040 1140 0000 A8C0 0100 A8C0 C700
  和 0x29E45 -> 回卷 0x9E47 -> 取反 = 0x61B8 = be16(0xB861)
```

注意 0x9E47 恰是 0x479E 的字节交换。一般化定理：**逐字字节交换的和 =
和的字节交换**（回卷恰好保证此性质），所以小端机上 csum16 的数值恒
等于网络序校验和的 be16。打出 BAD 恰恰说明你算对了。

三条规则（Phase 2/3 全部适用）：

1. **发送填校验和**：先 `hdr.checksum = 0;` 再
   `hdr.checksum = csum16(&hdr, 20);` -- 直接赋值，**不要 be16**。
   小端存储把"交换值"还原成正确线上字节（双重交换抵消）。
   DESIGN_IP §3.2 那句"结果再 bswap16 写入"只在**按字节逐个写包**
   （p[10]=hi, p[11]=lo）时成立；struct 赋值用它会双重交换，
   Wireshark 必报 incorrect checksum。
2. **收包验证**：`csum16(hdr, 20) == 0` 直接成立（含校验和字段；
   0x0000/0xFFFF 是回文，交换域不变）。
3. **当数值用**（自检/打印/比较）：先 `be16(csum16(...))`。

修复就一行：

```c
    if (be16(csum16(ip_csum_test, 20)) == 0xB861)
```

### 修改 `kernel/kernel.c`（两处）

include 区（其他 `#include "..."` 附近）加：

```c
#include "ip.h"
```

`e1000_init();` 之后（kernel.c:60 附近）加一行：

```c
    ip_init();
```

### 修改 `build_64_boot.sh`（三处）

1. CFLAGS 的 include 列表（行 42）末尾追加 `-I$BASE_DIR/net`
2. `e1000.o` 编译行（行 55）附近加一行：

```bash
gcc $CFLAGS -o "$OUTDIR/ip.o" net/ip.c
```

3. `KERNEL_OBJS`（行 72 附近）追加 `$OUTDIR/ip.o`

### 验证（Phase 1 完成）

```bash
bash build_64_boot.sh
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 -device e1000,netdev=net0 -m 1G
```

VGA 上 e1000 行之后应出现（DESIGN_IP §6 验证点 1）：

```
[e1000 ready] mac 52:54:00:12:34:56
[ip] 10.0.2.15/24 gw 10.0.2.2 csum ok
```

应用 be16 修复后仍打 `csum BAD` 的话按顺序排查：测试数组 20 字节抄错
（对照 DESIGN_IP §3.2 的字节串）、`(uint16_t)~sum` 截断没做、be16 笔误。
诊断技巧：把 csum16 原始结果用 utoa 打出来，x86 上应得 25016（0x61B8）。

### 常见编译错误对照

| 报错 | 原因 |
|---|---|
| `ip.h: No such file` | CFLAGS 忘加 `-I$BASE_DIR/net` |
| `undefined reference to ip_init` | 忘改 KERNEL_OBJS，或 kernel.c 忘加调用 |
| `print_newline 未声明` | 忘 include vga.h |

### 下一批

Phase 2 / 第 4 批：`net/arp.h` + `arp.c` 的缓存部分（查找/更新）。

---

## Phase 2 / 第 4 批：ARP 缓存核心（arp.h + arp.c 查找/更新）

本批只有缓存三件事：init / lookup / update。**不碰网卡、不碰帧**——
发请求（arp_request）和收应答（arp_handle）是后面两批。

### 新建 `long-mode/net/arp.h`

```c
#ifndef ARP_H
#define ARP_H

#include <stdint.h>

/* ARP 缓存（RFC 826），8 项静态数组，无 malloc。
 * struct 与数组封在 arp.c，外部只见函数。 */

void arp_init(void);

/* 纯查表：命中返回 1 并拷贝 mac_out；未命中返回 0（不发请求） */
int  arp_lookup(uint32_t ip, uint8_t mac_out[6]);

/* 插入/刷新一条（ip 主机序）。ip==0 忽略 */
void arp_update(uint32_t ip, const uint8_t mac[6]);

/* 后续批次再加：
 *   arp_resolve —— 查表 miss 则广播 request（供 ip_send 用）
 *   arp_handle  —— 收到 ARP 帧时更新缓存/应答（供 ip_poll 用） */

#endif
```

### 新建 `long-mode/net/arp.c`

```c
/* ARP cache（DESIGN_IP §3.3）。收发路径在后续批次。 */
#include "arp.h"
#include "lib.h"    /* memset / memcpy */

#define ARP_CACHE_N 8

struct arp_entry {
    uint32_t ip;      /* 主机序（bswap 约定见 ip.h） */
    uint8_t  mac[6];
    uint8_t  valid;
};

/* .bss 安全：exec_init 启动清零 [_kernel_load_end, _kernel_end)。
 * 对比：.netbuf 里的变量没这待遇，必须运行时显式清（见 ip.c 注释）。 */
static struct arp_entry arp_cache[ARP_CACHE_N];

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));  /* 显式清，防将来挪位置 */
}

int arp_lookup(uint32_t ip, uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

void arp_update(uint32_t ip, const uint8_t mac[6]) {
    if (ip == 0) return;   /* ARP probe 用 spa=0.0.0.0，不缓存垃圾 */

    /* 1. 已存在 -> 刷新 MAC（幂等：重复 reply 无副作用） */
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    /* 2. 顺序找第一个空位 */
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* 3. 全满 -> 覆盖 0 号（QEMU 场景实际 ≤2 项；要更公平可换 LRU） */
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
}
```

### 修改 `build_64_boot.sh`（两处）

仿照 Phase 1 的 ip.o：

1. 编译行（`ip.o` 那行后面）：
   `gcc $CFLAGS -o "$OUTDIR/arp.o" net/arp.c`
2. `KERNEL_OBJS` 追加 `$OUTDIR/arp.o`

`-I$BASE_DIR/net` Phase 1 已加过，所以 `#include "arp.h"` 直接能找到。

### 可选：3 分钟自检（临时代码，Phase 2 结束前删）

`ip.c` include 区加 `#include "arp.h"`；`ip_init` 里 csum 打印之后、
`print_newline` 之前插入：

```c
    arp_init();
    {
        uint8_t m[6]   = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC};
        uint8_t out[6];
        arp_update(IP_GW, m);
        if (arp_lookup(IP_GW, out) && !arp_lookup(IP4(1, 2, 3, 4), out))
            print_string(vga, " arp ok", &vga_cursor_pos);
        else
            print_string(vga, " arp BAD", &vga_cursor_pos);
    }
```

`arp_init();` 这行**永久保留**（它的最终家就是 ip_init）；测试块删掉。
加了自检则启动行变成：
`[ip] 10.0.2.15/24 gw 10.0.2.2 csum ok arp ok`

### 本批说明

- lookup 返回**拷贝**而非缓存内部指针：防止后续 arp_update 覆盖 0 号槽时
  指针悬空；单消费者场景下指针也安全，但拷贝更稳，教学取舍。
- 覆盖策略 3 步的顺序就是优先级：命中刷新 > 空位 > 牺牲 0 号。
- 本批不做 `arp_init` 的内核调用也没事（.bss 天然零），但下一批
  ip_send 要用缓存了，先在 ip_init 里挂上是正着。

### 下一批

见下方第 5 批。

---

## Phase 2 / 第 5 批：ARP 发送侧——第一个包出网卡

目标：`arp_resolve()` 未命中时广播一条 ARP request，pcap 里出现
"who has 10.0.2.2"，并从 slirp 的 reply 里**读出网关 MAC**（第 6 批要用）。

### 1. `net/bswap.h` 追加（`#endif` 之前）

```c
/* 主机序数值 <-> 网络序字节流。逐字节读写，不依赖缓冲对齐，
 * 适合操作帧内任意偏移的字段。host 序值的高位字节 = 线上首字节。 */
static inline void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
```

### 2. `net/arp.h` 修改

把"后续批次再加"的注释块替换为：

```c
/* 地址解析（供 ip_send 用）：命中返回 1 并填 mac_out；
 * 未命中广播 request、返回 0 —— 上层延时重试。
 * reply 填缓存是第 7 批 arp_handle 的事（无 pending 队列，
 * DESIGN_IP §4.4：避免复杂状态机）。 */
int  arp_resolve(uint32_t ip, uint8_t mac_out[6]);
```

### 3. `net/arp.c` 追加

include 区加两行（`lib.h` 之后）：

```c
#include "ip.h"     /* IP_ADDR：本机地址 */
#include "e1000.h"  /* e1000_send / e1000_get_mac */
```

文件末尾追加：

```c
/* ---- 发送侧（第 5 批）----------------------------------------- */

/* 广播 ARP request：who has <ip>, tell 本机。
 * 42 = 14 以太网头 + 28 ARP 报文；短帧由 e1000 的 TCTL_PSP 硬件 pad。 */
static void arp_request(uint32_t ip) {
    uint8_t frame[42];
    uint8_t *p = frame;
    const uint8_t *mac = e1000_get_mac();

    for (int i = 0; i < 6; i++) *p++ = 0xFF;    /* dst: 广播 */
    for (int i = 0; i < 6; i++) *p++ = mac[i];  /* src: 本机 */
    *p++ = 0x08; *p++ = 0x06;                   /* ethertype ARP */

    *p++ = 0x00; *p++ = 0x01;                   /* htype: ethernet */
    *p++ = 0x08; *p++ = 0x00;                   /* ptype: IPv4 */
    *p++ = 0x06; *p++ = 0x04;                   /* hlen 6, plen 4 */
    *p++ = 0x00; *p++ = 0x01;                   /* op: request */
    for (int i = 0; i < 6; i++) *p++ = mac[i];  /* sha */
    put_be32(p, IP_ADDR); p += 4;               /* spa: 10.0.2.15 */
    for (int i = 0; i < 6; i++) *p++ = 0x00;    /* tha: 未知填 0 */
    put_be32(p, ip);                            /* tpa: 要解析的地址 */

    e1000_send(frame, 42);
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    if (arp_lookup(ip, mac_out))
        return 1;
    arp_request(ip);
    return 0;   /* 请求已发出但不等应答；此刻还没人收 reply（第 7 批起） */
}
```

### 4. `net/ip.c` 修改（临时测试触发）

- include 区加 `#include "arp.h"`（第 4 批加过则跳过）
- `ip_init` 里**删掉第 4 批的假 MAC 自检块**（若做过），在 `arp_init();`
  之后插入：

```c
    /* [临时] 第 5 批：触发一次地址解析验证发送侧（第 7 批删） */
    {
        uint8_t mac[6];
        if (arp_resolve(IP_GW, mac))
            print_string(vga, " arp hit?!", &vga_cursor_pos);
        else
            print_string(vga, " arp sent", &vga_cursor_pos);
    }
```

（`arp_init()` 先清缓存，所以必走 miss 分支 → ` arp sent`。）

### 5. 验证

```bash
bash build_64_boot.sh
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -object filter-dump,id=f1,netdev=net0,file=/tmp/net.pcap -m 1G
```

1. 启动行出现 `[ip] 10.0.2.15/24 gw 10.0.2.2 csum ok arp sent`
   （若显示 `[no e1000]` 则先回 Phase 0 检查基线）。看到后关 QEMU。
2. 看 pcap（任选其一）：

```bash
tcpdump -r /tmp/net.pcap -nn -e     # 或用 Wireshark 打开
```

3. 预期两条（op=1 广播 / op=2 单播回给我们）：

```
FF:FF:FF:FF:FF:FF > Broadcast, ARP Request who-has 10.0.2.2 tell 10.0.2.15
52:55:0a:00:02:02 > 52:54:00:12:34:56, ARP Reply 10.0.2.2 is-at 52:55:0a:00:02:02
```

4. **记下 reply 的源 MAC**（slirp 网关 MAC；上例 52:55:0a:00:02:02，
   以你的 pcap 实际值为准）——第 6 批硬编码它来验证 ip_send。

### 本批说明

- `put_be32` 与 `be32` 殊途同归，但逐字节写不要求 4 字节对齐——帧内
  偏移 32 处的 spa 用"指针强转 + be32"在对齐严格的机器上是坑，按规范做。
- `arp_request` 保持 static：模块内部细节，对外只露 `arp_resolve`。
- 返回值分层：arp_resolve 返回 1/0（协议层不懂 errno），第 6 批 ip_send
  把 0 翻译成 IP_EBUSY——协议与错误码解耦。
- slirp 的 reply 此刻**没人收**（躺在 e1000 rx_pool 里），pcap 看得到
  是因为抓包点在虚拟网卡，与内核是否处理无关；第 7 批 ip_poll 才消费。
- 42 字节短帧不手动 pad：e1000_init 设了 TCTL_PSP，硬件兜底；
  第 6 批 ip_send 仍按 DESIGN_IP 规范手动 pad（不依赖硬件行为）。

### 下一批

见下方第 6 批。

---

## Phase 2 / 第 6 批：`ip_send`——第一个 IP 包出网卡

目标：填头、校验和、下一跳选择、组帧全部就位；预填网关 MAC 后发出
一条合法 ICMP echo request，**Wireshark 全绿（校验和 correct）**。
slirp 会回 echo reply——本批还没人收，那是第 7 批的饲料。

本批只改 `net/ip.c`，三处。

### 1. include 区加一行

```c
#include "e1000.h"  /* e1000_send / e1000_get_mac */
```

### 2. `ip_send` 桩替换为完整实现

```c
/* ---- 发送（DESIGN_IP §4.4）----
 * 返回 0 成功；IP_EBUSY = ARP 未解析（request 已广播，延时重试）；
 * IP_EINVAL = 参数错；-1 = e1000 发送失败（tx 环满，罕见）。 */
int32_t ip_send(uint8_t proto, uint32_t dst,
                const void *payload, uint16_t len) {
    if (len > IP_MAX_PAYLOAD || dst == 0 || (!payload && len))
        return IP_EINVAL;

    /* 下一跳：同网段直连，否则走网关（静态路由，§1"不做"表）。
     * 分层要点：IP 头写【最终目的】，以太网头写【下一跳的 MAC】。 */
    uint32_t nexthop = ((dst & IP_MASK) == (IP_ADDR & IP_MASK)) ? dst : IP_GW;

    uint8_t dst_mac[6];
    if (!arp_resolve(nexthop, dst_mac))
        return IP_EBUSY;             /* 上层稍后重试，下次缓存命中 */

    /* 帧布局：14 以太网头 + 20 IP 头 + payload(≤1480) = ≤1514。
     * 1514 栈帧 vs 4KB 内核栈：调用链浅（自测路径），安全。 */
    uint8_t frame[14 + 20 + IP_MAX_PAYLOAD];
    struct ip_hdr *h = (struct ip_hdr *)(frame + 14);
    const uint8_t *smac = e1000_get_mac();

    for (int i = 0; i < 6; i++) frame[i]     = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = smac[i];
    frame[12] = 0x08; frame[13] = 0x00;      /* ethertype: IPv4 */

    h->ver_ihl   = 0x45;                     /* v4 + 5 字（选项不做） */
    h->tos       = 0;
    h->total_len = be16((uint16_t)(20 + len));
    h->id        = be16(ip_id_counter++);    /* 自增，抓包对照用 */
    h->frag      = 0;                        /* 不分片（不支持） */
    h->ttl       = IP_TTL;
    h->proto     = proto;
    h->src       = be32(IP_ADDR);
    h->dst       = be32(dst);                /* 最终目的，不是 nexthop */

    h->checksum  = 0;                        /* 先清零！ */
    h->checksum  = csum16(h, 20);            /* 规则 1：直接赋值，勿 be16 */

    if (len)
        memcpy(frame + 34, payload, len);    /* 14 + 20 */

    uint16_t flen = 34 + len;
    if (flen < 60) {                         /* 以太网最小帧 */
        memset(frame + flen, 0, 60 - flen);
        flen = 60;
    }

    return (e1000_send(frame, flen) == 0) ? 0 : -1;
}
```

### 3. 测试块：删掉第 5 批的，原位置换上

```c
    /* [临时] 第 6 批：预填网关 MAC（第 5 批 pcap 里读到的），发一条
     * echo request 验证 ip_send（第 8 批删，届时 arp_handle 自动填） */
    {
        uint8_t gmac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 }; /* 换成你的！ */
        uint8_t req[8]  = { 8, 0, 0, 0, 0x12, 0x34, 0x00, 0x01 };
        /*                     type code csum(先0) id=0x1234 seq=1 */
        uint16_t c = csum16(req, 8);         /* 规则 1 字节流版 */
        req[2] = (uint8_t)(c & 0xFF);        /* c 是网络序值的 be16，
                                                其小端低字节 = 线上高字节 */
        req[3] = (uint8_t)(c >> 8);
        arp_update(IP_GW, gmac);             /* 命中路径（第 5 批测的是 miss） */
        int32_t r = ip_send(IP_PROTO_ICMP, IP_GW, req, 8);
        if (r == 0)
            print_string(vga, " ip ok", &vga_cursor_pos);
        else if (r == IP_EBUSY)
            print_string(vga, " ip busy", &vga_cursor_pos);
        else
            print_string(vga, " ip err", &vga_cursor_pos);
    }
```

### 4. 验证

```bash
bash build_64_boot.sh
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -object filter-dump,id=f1,netdev=net0,file=/tmp/net.pcap -m 1G
```

1. 启动行：`[ip] 10.0.2.15/24 gw 10.0.2.2 csum ok ip ok`
2. pcap 预期（**无 ARP 流量**——预填缓存走了命中路径）：

```
10.0.2.15 > 10.0.2.2  ICMP Echo request  id=0x1234, seq=1  (ttl 64, len 28)
10.0.2.2  > 10.0.2.15 ICMP Echo reply    id=0x1234, seq=1
```

3. Wireshark 逐层看：
   - IP 头：v4、header length 20、total 28、ttl 64、proto 1，
     **checksum 无 "incorrect" 标注**（验证点 3 达成）
   - ICMP：type 8 code 0，checksum correct
4. reply 此刻躺在 rx_pool 没人收（正常，第 7 批消费）

排错：
- `ip busy`：预填后必命中，busy 说明 `arp_update` 位置放错没执行到
- Wireshark 标 IP checksum incorrect：规则 1 破功——查是否先
  `h->checksum = 0`、赋值是否手滑加了 be16

### 本批说明

- **下一跳 vs 最终目的**是本批核心概念：以太网头管"这一跳交给谁"，
  IP 头管"最终给谁"。ping 网段外地址（如 IP4(8,8,8,8)）时 IP 头
  dst=8.8.8.8、以太网 dst=网关 MAC——第 9 批自测可试。
- 字节流版规则 1 与 struct 版殊途同归：`c`（交换值）的小端内存字节
  恰是线上字节，所以 `req[2] = c & 0xFF; req[3] = c >> 8;`。
- 42 字节帧手动 pad 到 60：不依赖 e1000 的 TCTL_PSP，按规范走。
- `ip_send` 是重试友好的：EBUSY 时上层稍等再调，第二次走缓存命中。

### 下一批

见下方第 7 批。

---

## Phase 2 / 第 7 批：ARP 接收侧——解析闭环（`ip_poll` + `arp_handle`）

目标：不预填任何 MAC，靠"广播 request → 轮询 `ip_poll` → `arp_handle`
吸收 reply"完成一次**全自动地址解析**。第 6 批的硬编码 MAC 从此退役。
启动行尾巴从 ` ip ok` 变成 ` arp ok`。

### 1. `net/arp.c` 修正：include 补一行（第 5 批遗漏）

```c
#include "bswap.h"  /* put_be32 / get_be32（第 5 批就该加） */
```

（若第 5 批编译时见过 `put_be32` 的警告/错误，根源在这。）

### 2. `net/arp.h` 追加声明

在 `arp_resolve` 声明之后加：

```c
/* 收到 ethertype 0x0806 帧时由 ip_poll 调用（frame 含 14B 以太网头）：
 * 先吸收发送者映射（RFC 826，request/reply 通吃）；
 * 若是问本机的 request 再单播回 reply；其余静默丢弃 */
void arp_handle(const uint8_t *frame, uint16_t len);
```

### 3. `net/arp.c` 追加（`arp_resolve` 之后）

```c
/* ---- 接收侧（第 7 批）----------------------------------------- */

/* 单播回一条 ARP reply 给请求者（帧布局同 arp_request，op=2） */
static void arp_reply(const uint8_t *sha, uint32_t spa) {
    uint8_t frame[42];
    uint8_t *p = frame;
    const uint8_t *mac = e1000_get_mac();

    for (int i = 0; i < 6; i++) *p++ = sha[i];    /* dst: 请求者，单播 */
    for (int i = 0; i < 6; i++) *p++ = mac[i];
    *p++ = 0x08; *p++ = 0x06;                     /* ethertype ARP */

    *p++ = 0x00; *p++ = 0x01;                     /* htype: ethernet */
    *p++ = 0x08; *p++ = 0x00;                     /* ptype: IPv4 */
    *p++ = 0x06; *p++ = 0x04;                     /* hlen 6, plen 4 */
    *p++ = 0x00; *p++ = 0x02;                     /* op: reply */
    for (int i = 0; i < 6; i++) *p++ = mac[i];    /* sha: 本机 */
    put_be32(p, IP_ADDR); p += 4;                 /* spa: 本机 IP */
    for (int i = 0; i < 6; i++) *p++ = sha[i];    /* tha: 请求者 */
    put_be32(p, spa);                             /* tpa: 请求者 IP */

    e1000_send(frame, 42);
}

/* ARP 报文在帧内的偏移（相对 14B 以太网头之后）：
 *   0-1 htype  2-3 ptype  4 hlen  5 plen  6-7 op
 *   8-13 sha   14-17 spa  18-23 tha  24-27 tpa           */
void arp_handle(const uint8_t *frame, uint16_t len) {
    if (len < 42)                                 /* 14 头 + 28 报文 */
        return;

    const uint8_t *a = frame + 14;
    if (a[0] != 0x00 || a[1] != 0x01 ||           /* htype: ethernet */
        a[2] != 0x08 || a[3] != 0x00 ||           /* ptype: IPv4 */
        a[4] != 6     || a[5] != 4)               /* hlen 6, plen 4 */
        return;                                   /* 不是 IPv4/eth，丢弃 */

    uint16_t    op  = ((uint16_t)a[6] << 8) | a[7];
    const uint8_t *sha = a + 8;
    uint32_t    spa = get_be32(a + 14);
    uint32_t    tpa = get_be32(a + 24);

    /* RFC 826 核心：先吸收发送者映射，request/reply 一条路径通吃 */
    arp_update(spa, sha);

    if (op == 1 && tpa == IP_ADDR)
        arp_reply(sha, spa);                      /* 有人问本机，单播回 */
    /* reply / 问别人的 request：吸收完映射就结束 */
}
```

### 4. `net/ip.c`：`ip_poll` 桩替换

```c
/* ---- 接收分发（DESIGN_IP §4.5）----
 * 消费 e1000 rx_pool 里的所有帧，按 ethertype 分发。
 * 只在单上下文跑（自测轮询）——无锁设计的根基。 */
void ip_poll(void) {
    uint8_t frame[2048];                          /* = e1000 BUF_SIZE */
    int32_t n;

    while ((n = e1000_recv(frame, sizeof(frame))) > 0) {
        if (n < 14)
            continue;                             /* 残帧 */
        uint16_t et = ((uint16_t)frame[12] << 8) | frame[13];
        if (et == 0x0806)
            arp_handle(frame, (uint16_t)n);
        /* et == 0x0800 -> ip_handle() 第 8 批；其他 ethertype 丢弃 */
    }
}
```

### 5. `net/ip.c`：测试块替换（删第 6 批的，原位置换上）

```c
    /* [临时] 第 7 批：全自动 ARP 闭环（第 9 批并入 ip_selftest） */
    {
        uint8_t mac[6];
        int hit = 0;
        arp_resolve(IP_GW, mac);                  /* miss → 已广播 request */
        for (int round = 0; round < 200 && !hit; round++) {
            for (volatile uint32_t spin = 0; spin < 3000000; spin++);
            ip_poll();                            /* 消费 rx_pool */
            hit = arp_lookup(IP_GW, mac);
        }
        print_string(vga, hit ? " arp ok" : " arp timeout", &vga_cursor_pos);
    }
```

### 6. 验证

```bash
bash build_64_boot.sh
qemu-system-x86_64 -drive format=raw,file=output/disk.img \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -object filter-dump,id=f1,netdev=net0,file=/tmp/net.pcap -m 1G
```

1. 启动行：`[ip] 10.0.2.15/24 gw 10.0.2.2 csum ok arp ok`
2. pcap：只有一对 ARP request/reply（第 6 批的 echo 测试已删）
3. ` arp ok` 只可能来自 `arp_handle` 里的 `arp_update`——**打印即证明
   接收路径通**（lookup 命中的数据源只有这一条路）

排错（` arp timeout` 时）：

1. 先在 `arp_handle` 入口临时加 `print_string(vga, "[arp] ", &vga_cursor_pos);`
   确认它被调到；被调到但不命中 → 逐字段核对解析偏移（对照
   Wireshark 里 reply 的字节）；没被调到 → `et` 判断或 `n` 有问题
2. reply 根本没进 rx_pool → e1000 中断问题，回 Phase 0 基线
3. 调试完记得删临时打印

### 本批说明

- **RFC 826 "先吸收再判断"**：任何校验通过的 ARP 帧先 `arp_update(spa,
  sha)`，op 只决定"要不要回话"。request/reply 共用一条路径，无状态机。
- reply 是**单播**（dst = 请求者 MAC），不再广播。
- `ip_poll` 是 rx_pool 的唯一消费者，跑在启动上下文（ip_init）——
  若将来挂到别处（如 timer），必须保证仍是唯一消费者。
- `spin 3M` 的意义：给 e1000 IRQ 时间把 reply 搬进 rx_pool；
  `ip_poll` 读的是 rx_pool，不是网卡 MMIO。
- `arp_resolve` 第一次 miss 会发 request，测试循环里第二次调用
  `arp_lookup` 命中后循环即止——这就是将来 `ip_send` 重试 EBUSY 的
  同款节奏。

### 下一批

第 8 批：`ip_handle`（IP 包校验/分片丢弃/投递队列）+ `ip_rx` 队列
（`.netbuf`）+ `icmp.c`（echo 自动应答）——发一条 echo request 并真的
收到 reply，**最小收发闭环**。

---

## 路线图：第 6-10 批（Phase 2 收尾 → 收发闭环 → 抓包验证）

> **范围调整**：最终目标 = 内核态 IP 完整收发。用户态 syscall 接口
> （SYS_IPSEND/IPRECV/IPINFO、userlib、ping.c）**不做**。验证全部走
> 内核自测 + QEMU pcap。

| 批 | 主题 | 内容 | 验证方式 |
|---|---|---|---|
| 6 | `ip_send` 实现 | 静态路由下一跳 + ARP + 填头 + 校验和规则 1 + pad 60 | 预填网关 MAC 发 echo，Wireshark 校验和 correct |
| 7 | ARP 接收侧 | `ip_poll` 骨架（ethertype 分发）+ `arp_handle`（request 应答 / reply 填缓存） | request → poll → lookup 命中，解析闭环 |
| 8 | IP 接收 + ICMP | `ip_handle`（校验/分片丢弃/投递）+ `ip_rx` 队列(.netbuf) + icmp.c 应答与发送 | 最小闭环：发 echo → 轮询 → 收 reply 打印 |
| 9 | 闭环整合 | `ip_selftest`：ARP miss 重试、多 seq、超时、`[arp] cache hit` | 启动即完成一次完整收发演示 |
| 10 | 抓包验证 + 清理 | DESIGN_IP §6 清单（内核自测适配版）；删临时代码 | 校验和无 incorrect、二次启动无 ARP、可选 UDP→:9 收 type=3 |

每批关键点：

- **第 6 批**：网关 MAC 不猜，从第 5 批 pcap 的 ARP reply 源地址读出。
- **第 7 批**：`ip_poll` 严格单上下文（自测轮询循环）——无锁设计根基，
  两个消费者会竞争 rx_pool 的 head/tail。
- **第 8 批**：兑现决策 C——`ip_rx_bufs` 进 `.netbuf`、`ip_init` 显式
  清零；echo request 发送 helper 也放 icmp.c（没有 ping.c，内核自发）。
- **第 9 批**：忙等节奏沿用 net.c 的 3M 次循环；网段外地址（如
  8.8.8.8）顺带验证下一跳逻辑。
- **第 10 批**：UDP discard 测试的 UDP 校验和可为 0（IPv4 合法）。

### 当前待办

1. 敲第 7 批（上方详情），启动行出现 ` arp ok`
