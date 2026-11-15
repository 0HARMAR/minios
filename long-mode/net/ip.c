#include "ip.h"
#include "bswap.h"
#include "lib.h"
#include "vga.h"
#include "arp.h"
#include "e1000.h"
#include "icmp.h"
#include "tcp.h"

extern uint32_t vga_cursor_pos;
static const uint8_t ip_csum_test[20] = {
    0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
    0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
    0xC0, 0xA8, 0x00, 0xC7,
};

static uint16_t ip_id_counter;
static void print_ip(unsigned char *vga, uint32_t ip);

void ip_init(void) {
    unsigned char *vga = (unsigned char *)VGA_BUFFER;

    print_string(vga, "[ip] ", &vga_cursor_pos);
    print_ip(vga, IP_ADDR);
    print_string(vga, "/24 gw", &vga_cursor_pos);
    print_ip(vga, IP_GW);

    if (be16(csum16(ip_csum_test, 20)) == 0xB861)
        print_string(vga, " csum ok", &vga_cursor_pos);
    else
        print_string(vga, " csum BAD", &vga_cursor_pos);

    arp_init();
    // // arp test
    // {
    //     uint8_t m[6] = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC};
    //     uint8_t out[6];
    //     arp_update(IP_GW, m);
    //     if (arp_lookup(IP_GW, out) && !arp_lookup(IP4(1, 2, 3, 4), out))
    //         print_string(vga, " arp ok", &vga_cursor_pos);
    //     else
    //         print_string(vga, " arp BAD", &vga_cursor_pos);
    // }

    // // arp resolve test
    // {
    //     uint8_t mac[6];
    //     if (arp_resolve(IP_GW, mac))
    //         print_string(vga, " arp hit?!:", &vga_cursor_pos);
    //     else
    //         print_string(vga, " arp sent", &vga_cursor_pos);
    // }

    // // ip send test
    // {
    //     uint8_t gmac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    //     uint8_t req[8] = {8, 0, 0, 0, 0x12, 0x34, 0x00, 0x01};
    //     uint16_t c = csum16(req, 8);
    //     req[2] = (uint8_t)(c & 0xFF);
    //     req[3] = (uint8_t)(c >> 8);
    //     arp_update(IP_GW, gmac);
    //     int32_t r = ip_send(IP_PROTO_UDP, IP_GW, req, 8);
    //     if (r == 0)
    //         print_string(vga, " ip ok", &vga_cursor_pos);
    //     else if (r == IP_EBUSY)
    //         print_string(vga, " ip busy", &vga_cursor_pos);
    //     else
    //         print_string(vga, " ip BAD", &vga_cursor_pos);
    // }

    {
        uint8_t mac[6];
        int hit = 0;
        arp_resolve(IP_GW, mac);
        for (int round = 0; round < 200 && !hit; round++) {
            for (volatile uint32_t spin = 0; spin < 3000000; spin++);
            ip_poll();
            hit = arp_lookup(IP_GW, mac);
        }
        print_string(vga, hit ? " arp ok" : " arp timeout", &vga_cursor_pos);
    }
    print_newline(vga, &vga_cursor_pos);
    update_cursor(vga_cursor_pos);

    ip_id_counter = 0;
}


int32_t ip_send(uint8_t proto, uint32_t dst, const void* payload, uint16_t len)
{
    if (len > IP_MAX_PAYLOAD || dst == 0 || (!payload && len))
        return IP_EINVAL;

    // 下一跳：同网段直连，否则走网关
    uint32_t nexthop = ((dst & IP_MASK) == (IP_ADDR & IP_MASK)) ? dst : IP_GW;

    uint8_t dst_mac[6];
    if (!arp_resolve(nexthop, dst_mac))
        return IP_EBUSY;

    // 14以太网头，20 ip头
    uint8_t frame[14 + 20 + IP_MAX_PAYLOAD];
    struct ip_hdr *h = (struct ip_hdr *)(frame + 14);
    const uint8_t *smac = e1000_get_mac();

    for (int i = 0; i < 6; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = smac[i];
    frame[12] = 0x08; frame[13] = 0x00; // type: IPv4

    h->ver_ihl = 0x45;
    h->tos = 0;
    h->total_len = be16((uint16_t)(20 + len));
    h->id = be16(ip_id_counter++);
    h->frag = 0;
    h->ttl = IP_TTL;
    h->proto = proto;
    h->src = be32(IP_ADDR);
    h->dst = be32(dst);
    h->checksum = 0;
    h->checksum = csum16(h, 20);

    if (len)
        memcpy(frame + 14 + 20, payload, len);

    uint16_t flen = (uint16_t)(14 + 20 + len);
    if (flen < 60) {
        memset(frame + flen, 0, 60 - flen);
        flen = 60;
    }

    return (e1000_send(frame, flen) == 0) ? 0 : -1;
}

/* 同网段源地址顺手学习 MAC，避免回 ICMP 时 ARP 未命中。 */
static void ip_learn_src(const uint8_t *frame, uint32_t src)
{
    if (src == 0 || src == IP_ADDR || src == IP_BCAST)
        return;
    if ((src & 0xF0000000u) == 0xE0000000u)     /* 组播源 */
        return;
    if ((src & IP_MASK) != (IP_ADDR & IP_MASK)) /* 非同网段：源 MAC 是网关的 */
        return;
    if (frame[6] & 0x01)                        /* 组播/广播 MAC */
        return;

    int zero = 1;
    for (int i = 0; i < 6; i++)
        if (frame[6 + i])
            zero = 0;
    if (zero)
        return;

    arp_update(src, frame + 6);
}

/* 生产版 IP 入口：校验 + 分片丢弃 + 按协议分发。 */
static void ip_handle(uint8_t *frame, uint16_t n)
{
    if (n < 14 + 20)
        return;

    struct ip_hdr *h = (struct ip_hdr *)(frame + 14);
    uint16_t avail = (uint16_t)(n - 14);
    uint16_t ihl = (uint16_t)((h->ver_ihl & 0x0F) * 4);
    uint16_t total = be16(h->total_len);

    if ((h->ver_ihl >> 4) != 4 || ihl < 20 || total < ihl || total > avail)
        return;
    if (csum16(h, ihl) != 0)
        return;
    if (be16(h->frag) & 0x3FFF)   /* MF 或 offset != 0：分片，暂不支持 */
        return;

    uint32_t dst = be32(h->dst);
    if (dst != IP_ADDR && dst != IP_BCAST)
        return;

    ip_learn_src(frame, be32(h->src));

    if (h->proto == IP_PROTO_ICMP)
        icmp_input((uint8_t *)h, ihl, total);
    else if (h->proto == IP_PROTO_TCP)
        tcp_input((uint8_t *)h, ihl, total);
}

void ip_poll(void)
{
    uint8_t frame[2048];
    int32_t n;

    while ((n = e1000_recv(frame, sizeof(frame))) > 0) {
        if (n < 14)
            continue;
        uint16_t et = ((uint16_t)frame[12] << 8) | frame[13];
        if (et == 0x0806) {
            arp_handle(frame, (uint16_t)n);
        } else if (et == 0x0800) {
            ip_handle(frame, (uint16_t)n);
        }
    }
}

int32_t ip_recv(uint8_t* out, uint16_t maxlen)
{
    (void)out; (void)maxlen;
    return 0;
}

uint16_t csum16(const void* data, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {sum += *p++; len -= 2;}
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void print_ip(unsigned char *vga, uint32_t ip) {
    char tmp[4];
    for (int i = 3; i >= 0; i--) {
        utoa((uint32_t)((ip >> (i * 8)) & 0xFF), tmp);
        print_string(vga, tmp, &vga_cursor_pos);
        if (i) print_string(vga, ".", &vga_cursor_pos);
    }
}
