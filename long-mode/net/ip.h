#ifndef IP_H
#define IP_H

#include <stdint.h>

/* ── 接收侧调试模式 ──────────────────────────────────────────
 * 1 = 不启动 ush：内核发一个 ICMP echo 后死循环 ip_poll，
 *     每个通过校验的 IP 包打一行到 COM1；测完改回 0。 */
#ifndef IP_TEST_MODE
#define IP_TEST_MODE 1
#endif

#define IP4(a, b, c, d) (((uint32_t)(a)<<24) | ((uint32_t)(b)<<16) | ((uint32_t)(c)<<8) | (uint32_t)(d))
#define IP_ADDR IP4(10, 0, 2, 16) // 本机
#define IP_MASK IP4(255, 255, 255, 0)
#define IP_GW IP4(10, 0, 2, 2) // slirp 网关
#define IP_BCAST 0XFFFFFFFFu // 255.255.255.255

// 协议常量
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define IP_TTL 64
#define IP_MAX_PAYLOAD 1480 // 1500 MTU - 20头

#define IP_EINVAL (-5)
#define IP_EBUSY (-6)

struct ip_hdr {
    uint8_t ver_ihl; // 0x45
    uint8_t tos;
    uint16_t total_len; // 头加载荷
    uint16_t id; // 发送时全局自增
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

uint16_t csum16(const void* data, uint32_t len);
void ip_init();
int32_t ip_send(uint8_t proto, uint32_t dst, const void* payload, uint16_t len);
void ip_poll(void);
int32_t ip_recv(uint8_t* out, uint16_t maxlen);

#if IP_TEST_MODE
/* 发一个 ICMP echo 到网关，给接收侧制造入站流量 */
void ip_test_send_echo(void);
#endif

#endif