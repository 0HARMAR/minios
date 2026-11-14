#include "arp.h"
#include "lib.h"
#include "ip.h"
#include "e1000.h"
#include "bswap.h"

#define ARP_CACHE_N 8

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;
};

static struct arp_entry arp_cache[ARP_CACHE_N];

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
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
    if (ip == 0) return;
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }

    // 顺序找第一个空位
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }

    // 全满
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
}

static void arp_request(uint32_t ip) {
    uint8_t frame[42];
    uint8_t *p = frame;
    const uint8_t *mac = e1000_get_mac();

    for (int i = 0; i < 6; i++) *p++ = 0xFF; // dst: broadcast
    for (int i = 0; i < 6; i++) *p++ = mac[i]; // src: local
    *p++ = 0x08; *p++ = 0x06; // type: ARP

    *p++ = 0x00; *p++ = 0x01; // hw type: ethernet
    *p++ = 0x08; *p++ = 0x00; // proto type: IPv4
    *p++ = 0x06; *p++ = 0x04; // hw size: 6, proto size: 4
    *p++ = 0x00; *p++ = 0x01; // opcode: request
    for (int i = 0; i < 6; i++) *p++ = mac[i]; // sha
    put_be32(p, IP_ADDR); p += 4; // spa: 10.0.2.15
    for (int i = 0; i < 6; i++) *p++ = 0x00; // tha
    put_be32(p, ip);

    e1000_send(frame, sizeof(frame));
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    if (arp_lookup(ip, mac_out)) return 1;
    arp_request(ip);
    return 0;
}

// sha : sender hardware address
static void arp_reply(const uint8_t *sha, uint32_t spa) {
    uint8_t frame[42];
    uint8_t *p = frame;
    const uint8_t *mac = e1000_get_mac();

    for (int i = 0; i < 6; i++) *p++ = sha[i];
    for (int i = 0; i < 6; i++) *p++ = mac[i];
    *p++ = 0x08; *p++ = 0x06;
    *p++ = 0x00; *p++ = 0x01;
    *p++ = 0x08; *p++ = 0x00;
    *p++ = 0x06; *p++ = 0x04;
    *p++ = 0x00; *p++ = 0x02;
    for (int i = 0; i < 6; i++) *p++ = mac[i];
    put_be32(p, IP_ADDR); p += 4;
    for (int i = 0; i < 6; i++) *p++ = sha[i];
    put_be32(p, spa);
    e1000_send(frame, sizeof(frame));
}

void arp_handle(const uint8_t *frame, uint16_t len) {
    if (len < 42)
        return;

    const uint8_t *a = frame + 14;
    if (a[0] != 0x00 || a[1] != 0x01 || a[2] != 0x08 || a[3] != 0x00 || a[4] != 0x06 || a[5] != 0x04)
        return;

    uint16_t op = ((uint16_t)a[6] << 8) | a[7];
    const uint8_t *sha = a + 8;
    uint32_t spa = get_be32(a + 14); // spa : sender protocol address(ip)
    uint32_t tpa = get_be32(a + 24); // tpa : target protocol address(ip)

    arp_update(spa, sha);

    if (op == 1 && tpa == IP_ADDR) {
        arp_reply(sha, spa);
    }
}
