#include "icmp.h"
#include "ip.h"
#include "bswap.h"
#include "lib.h"
#include "syscall.h"   /* serial_write，仅 IP_TEST_MODE 日志用 */
#include "timer.h"

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

/* 令牌桶：10 个/秒，突发 10（1 token = 10 ticks = 100ms） */
#define ICMP_BUCKET_MAX    10
#define ICMP_REFILL_TICKS  10

static uint32_t icmp_tokens = ICMP_BUCKET_MAX;
static uint32_t icmp_last_tick;
static uint32_t icmp_req_count;
static uint32_t icmp_reply_count;
static uint32_t icmp_drop_rate;
static uint32_t icmp_bad;

#if IP_TEST_MODE
static void dbg_str(const char *s)
{
    serial_write(s, strlen(s));
}

static void dbg_dec(uint32_t v)
{
    char b[12];
    utoa(v, b);
    dbg_str(b);
}

static void dbg_ip(uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        dbg_dec((ip >> (i * 8)) & 0xFF);
        if (i)
            dbg_str(".");
    }
}
#endif

static int icmp_take_token(void)
{
    uint32_t now = (uint32_t)timer_get_ticks();
    uint32_t elapsed = now - icmp_last_tick;

    if (elapsed >= 100) {
        icmp_tokens = ICMP_BUCKET_MAX;
        icmp_last_tick = now;
    } else {
        while (elapsed >= ICMP_REFILL_TICKS) {
            if (icmp_tokens < ICMP_BUCKET_MAX)
                icmp_tokens++;
            icmp_last_tick += ICMP_REFILL_TICKS;
            elapsed -= ICMP_REFILL_TICKS;
        }
    }

    if (icmp_tokens == 0)
        return 0;
    icmp_tokens--;
    return 1;
}

void icmp_input(uint8_t *ip, uint16_t ihl, uint16_t total)
{
    struct ip_hdr *h = (struct ip_hdr *)ip;
    uint8_t *icmp = ip + ihl;
    uint16_t icmp_len = (uint16_t)(total - ihl);

    if (icmp_len < 8) {
        icmp_bad++;
        return;
    }
    if (icmp[0] != ICMP_ECHO_REQUEST || icmp[1] != 0)
        return;
    if (csum16(icmp, icmp_len) != 0) {
        icmp_bad++;
        return;
    }

    uint32_t src = be32(h->src);
    uint32_t dst = be32(h->dst);

    /* 只回单播；拒绝非法/自环/广播/组播源 */
    if (dst != IP_ADDR)
        return;
    if (src == 0 || src == IP_ADDR || src == IP_BCAST)
        return;
    if ((src & 0xF0000000u) == 0xE0000000u)
        return;

    if (!icmp_take_token()) {
        icmp_drop_rate++;
        return;
    }

    icmp_req_count++;

#if IP_TEST_MODE
    uint16_t seq = (uint16_t)((icmp[6] << 8) | icmp[7]);
    dbg_str("[icmp] req ");
    dbg_ip(src);
    dbg_str(" > ");
    dbg_ip(dst);
    dbg_str(" seq=");
    dbg_dec(seq);
    dbg_str(" len=");
    dbg_dec(total);
    dbg_str("\n");
#endif

    /* 原地改成 echo reply：type=0, code=0，校验和清零后重算 */
    icmp[0] = ICMP_ECHO_REPLY;
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t c = csum16(icmp, icmp_len);
    icmp[2] = (uint8_t)(c & 0xFF);
    icmp[3] = (uint8_t)(c >> 8);

    if (ip_send(IP_PROTO_ICMP, src, icmp, icmp_len) == 0) {
        icmp_reply_count++;
#if IP_TEST_MODE
        dbg_str("[icmp] reply > ");
        dbg_ip(src);
        dbg_str(" seq=");
        dbg_dec(seq);
        dbg_str(" len=");
        dbg_dec(total);
        dbg_str("\n");
#endif
    }
}
