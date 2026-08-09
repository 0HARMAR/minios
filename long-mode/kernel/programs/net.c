#include <stdint.h>
#include "../userlib.h"

#define ETH_ARP  0x0806
#define ETH_IP   0x0800

static void puthex8(uint8_t b, char *out) {
    static const char hex[] = "0123456789abcdef";
    out[0] = hex[b >> 4];
    out[1] = hex[b & 0xF];
}

static void print_mac(uint8_t *mac) {
    char buf[3];
    for (int i = 0; i < 6; i++) {
        puthex8(mac[i], buf);
        buf[2] = 0;
        output(0, buf, 2);
        if (i < 5) output(0, ":", 1);
    }
}

static uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}

/* build an ARP request asking "who has 10.0.2.2" */
static void build_arp_request(uint8_t *frame, uint8_t *mac) {
    uint8_t *p = frame;

    for (int i = 0; i < 6; i++) *p++ = 0xFF;              /* dst: broadcast */
    for (int i = 0; i < 6; i++) *p++ = mac[i];            /* src: our MAC */
    *p++ = ETH_ARP >> 8; *p++ = ETH_ARP & 0xFF;           /* ethertype */

    *p++ = 0x00; *p++ = 0x01;                             /* htype: ethernet */
    *p++ = 0x08; *p++ = 0x00;                             /* ptype: IPv4 */
    *p++ = 0x06; *p++ = 0x04;                             /* hlen 6, plen 4 */
    *p++ = 0x00; *p++ = 0x01;                             /* op: request */
    for (int i = 0; i < 6; i++) *p++ = mac[i];            /* sender MAC */
    *p++ = 10; *p++ = 0; *p++ = 2; *p++ = 15;             /* sender IP 10.0.2.15 */
    for (int i = 0; i < 6; i++) *p++ = 0x00;              /* target MAC 0 */
    *p++ = 10; *p++ = 0; *p++ = 2; *p++ = 2;              /* target IP 10.0.2.2 */
}

static void hexdump(uint8_t *p, uint16_t n) {
    char line[8];
    if (n > 64) n = 64;
    for (uint16_t i = 0; i < n; i++) {
        if (i % 16 == 0) output(0, "\n  ", 3);
        puthex8(p[i], line);
        line[2] = 0;
        output(0, line, 2);
        output(0, " ", 1);
    }
    output(0, "\n", 1);
}

void _start(void) {
    output(0, "[net] test started\n", 19);

    uint8_t mac[6];
    if (netmac(mac, 6) != 6) {
        output(0, "[net] no nic!\n", 14);
        exit(1);
    }

    output(0, "[net] mac ", 10);
    print_mac(mac);
    output(0, "\n", 1);

    output(0, "[net] sending ARP request...\n", 30);

    uint8_t frame[64];
    build_arp_request(frame, mac);
    int64_t sent = netsend(frame, 42);
    if (sent < 0) {
        output(0, "[net] send failed\n", 18);
        exit(1);
    }
    output(0, "[net] arp sent, waiting for replies\n", 36);

    uint8_t rbuf[1600];
    for (int round = 0; round < 200; round++) {
        /* busy-wait: QEMU's slirp answers ARP within a few ms, but a
         * non-blocking poll would exhaust all rounds instantly */
        for (volatile uint32_t spin = 0; spin < 3000000; spin++);
        int64_t n = netrecv(rbuf, sizeof(rbuf));
        if (n > 0) {
            output(0, "[net] RX ", 9);
            char num[12];
            uint32_t len = 0;
            uint32_t t = (uint32_t)n;
            do { num[len++] = '0' + (char)(t % 10); t /= 10; } while (t);
            for (uint32_t i = 0; i < len / 2; i++) {
                char c = num[i]; num[i] = num[len - 1 - i]; num[len - 1 - i] = c;
            }
            num[len] = 0;
            output(0, num, len);
            output(0, " bytes:", 7);
            hexdump(rbuf, (uint16_t)n);

            if (n >= 14 && rbuf[12] == 0x08 && rbuf[13] == 0x06) {
                output(0, "[net] ARP reply! ", 17);
                print_mac(rbuf + 6);
                output(0, "\n[net] done\n", 12);
                exit(0);
            }
        }
    }

    output(0, "[net] no reply within timeout\n", 30);
    exit(0);
}
