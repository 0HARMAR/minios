#include "tcp.h"
#include "ip.h"
#include "bswap.h"
#include "lib.h"
#include "syscall.h"   /* serial_write，仅 IP_TEST_MODE 日志用 */
#include "timer.h"

#define TCP_TCB_N 4

static struct tcp_tcb tcb_pool[TCP_TCB_N];
static uint32_t tcp_isn_counter;

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

static void dbg_hex8(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    char b[4] = { '0', 'x', hex[v >> 4], hex[v & 0x0F] };
    serial_write(b, 4);
}
#endif

static uint32_t tcp_isn(void)
{
    uint32_t v = (uint32_t)timer_get_ticks() * 64000u
               + (tcp_isn_counter++ << 12)
               + 0x12345u;
    return v ? v : 1u;
}

/* 16 位反码和，按网络字节序累加；奇数尾字节视为高字节。 */
static uint32_t csum_partial(const uint8_t *p, uint32_t len, uint32_t sum)
{
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

/* src/dst 是主机序；返回主机序的校验和值。合法报文（含校验和）算出来为 0。 */
static uint16_t tcp_checksum(uint32_t src, uint32_t dst,
                             const uint8_t *tcp, uint16_t len)
{
    uint8_t pseudo[12];

    put_be32(pseudo, src);
    put_be32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)len;

    uint32_t sum = csum_partial(pseudo, 12, 0);
    sum = csum_partial(tcp, len, sum);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_parse_mss(const uint8_t *tcp, uint16_t hlen, uint16_t len)
{
    uint16_t mss = 536;
    uint16_t i = 20;

    while (i < hlen && i < len) {
        uint8_t kind = tcp[i];
        if (kind == 0)              /* End of option list */
            break;
        if (kind == 1) {            /* NOP */
            i++;
            continue;
        }
        if (i + 1 >= len)
            break;
        uint8_t olen = tcp[i + 1];
        if (olen < 2 || i + olen > hlen || i + olen > len)
            break;
        if (kind == 2 && olen == 4) {
            mss = (uint16_t)((tcp[i + 2] << 8) | tcp[i + 3]);
            if (mss > TCP_MSS)
                mss = TCP_MSS;
            if (mss < 64)
                mss = 64;
        }
        i += olen;
    }
    return mss;
}

static struct tcp_tcb *tcp_alloc(void)
{
    for (int i = 0; i < TCP_TCB_N; i++) {
        if (tcb_pool[i].state == TCP_CLOSED) {
            memset(&tcb_pool[i], 0, sizeof(tcb_pool[i]));
            return &tcb_pool[i];
        }
    }
    return 0;
}

int tcp_listen(uint16_t port)
{
    for (int i = 0; i < TCP_TCB_N; i++) {
        if (tcb_pool[i].state == TCP_LISTEN &&
            tcb_pool[i].local_port == port)
            return 0;
        if (tcb_pool[i].state == TCP_CLOSED) {
            memset(&tcb_pool[i], 0, sizeof(tcb_pool[i]));
            tcb_pool[i].state = TCP_LISTEN;
            tcb_pool[i].local_ip = IP_ADDR;
            tcb_pool[i].local_port = port;
            tcb_pool[i].rcv_wnd = TCP_WINDOW;
            tcb_pool[i].mss = TCP_MSS;
            return 0;
        }
    }
    return -1;
}

/* 发一个不带数据的 TCP 段；with_mss 只在 SYN/SYN-ACK 时为 1。 */
static int32_t tcp_emit(struct tcp_tcb *t, uint8_t flags,
                        uint32_t seq, uint32_t ack, int with_mss)
{
    uint8_t seg[24];
    uint16_t hlen = with_mss ? 24 : 20;

    memset(seg, 0, sizeof(seg));
    seg[0] = (uint8_t)(t->local_port >> 8);
    seg[1] = (uint8_t)t->local_port;
    seg[2] = (uint8_t)(t->remote_port >> 8);
    seg[3] = (uint8_t)t->remote_port;
    put_be32(seg + 4, seq);
    put_be32(seg + 8, ack);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    seg[14] = (uint8_t)(t->rcv_wnd >> 8);
    seg[15] = (uint8_t)t->rcv_wnd;
    /* seg[16..19]: checksum + urgent，已由 memset 清零 */

    if (with_mss) {
        seg[20] = 2;                /* MSS option */
        seg[21] = 4;
        seg[22] = (uint8_t)(TCP_MSS >> 8);
        seg[23] = (uint8_t)TCP_MSS;
    }

    uint16_t c = tcp_checksum(IP_ADDR, t->remote_ip, seg, hlen);
    seg[16] = (uint8_t)(c >> 8);
    seg[17] = (uint8_t)c;

    return ip_send(IP_PROTO_TCP, t->remote_ip, seg, hlen);
}

void tcp_input(uint8_t *ip, uint16_t ihl, uint16_t total)
{
    struct ip_hdr *h = (struct ip_hdr *)ip;
    uint8_t *tcp = ip + ihl;
    uint16_t len = (uint16_t)(total - ihl);

    if (len < 20)
        return;

    uint16_t sport = (uint16_t)((tcp[0] << 8) | tcp[1]);
    uint16_t dport = (uint16_t)((tcp[2] << 8) | tcp[3]);
    uint32_t seq = get_be32(tcp + 4);
    uint32_t ack = get_be32(tcp + 8);
    uint8_t off = (uint8_t)((tcp[12] >> 4) * 4);
    uint8_t flags = tcp[13];
    uint16_t wnd = (uint16_t)((tcp[14] << 8) | tcp[15]);

    if (off < 20 || off > len)
        return;

    uint32_t src = be32(h->src);
    uint32_t dst = be32(h->dst);
    if (tcp_checksum(src, dst, tcp, len) != 0)
        return;

    /* 先找完全匹配的连接，再回落到 LISTEN。 */
    struct tcp_tcb *t = 0;
    for (int i = 0; i < TCP_TCB_N; i++) {
        if (tcb_pool[i].state != TCP_CLOSED &&
            tcb_pool[i].state != TCP_LISTEN &&
            tcb_pool[i].local_port == dport &&
            tcb_pool[i].remote_port == sport &&
            tcb_pool[i].remote_ip == src) {
            t = &tcb_pool[i];
            break;
        }
    }

    if (!t) {
        for (int i = 0; i < TCP_TCB_N; i++) {
            if (tcb_pool[i].state == TCP_LISTEN &&
                tcb_pool[i].local_port == dport) {
                t = &tcb_pool[i];
                break;
            }
        }
    }
    if (!t)
        return;

    if (t->state == TCP_LISTEN) {
        if (!(flags & TCP_SYN) || (flags & (TCP_ACK | TCP_RST)))
            return;

        struct tcp_tcb *c = tcp_alloc();
        if (!c)
            return;

        c->local_ip = IP_ADDR;
        c->remote_ip = src;
        c->local_port = dport;
        c->remote_port = sport;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = tcp_isn();
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = wnd;
        c->rcv_wnd = TCP_WINDOW;
        c->mss = tcp_parse_mss(tcp, off, len);
        c->state = TCP_SYN_RCVD;

#if IP_TEST_MODE
        dbg_str("[tcp] SYN ");
        dbg_ip(src);
        dbg_str(":");
        dbg_dec(sport);
        dbg_str(" -> :");
        dbg_dec(dport);
        dbg_str(" seq=");
        dbg_dec(seq);
        dbg_str(" flags=");
        dbg_hex8(flags);
        dbg_str("\n");
#endif

        int32_t r = tcp_emit(c, TCP_SYN | TCP_ACK, c->iss, c->rcv_nxt, 1);
#if IP_TEST_MODE
        dbg_str("[tcp] SYN-ACK seq=");
        dbg_dec(c->iss);
        dbg_str(" ack=");
        dbg_dec(c->rcv_nxt);
        dbg_str(" flags=");
        dbg_hex8(TCP_SYN | TCP_ACK);
        if (r != 0)
            dbg_str(" send-fail");
        dbg_str("\n");
#endif
        (void)r;
        return;
    }

    if (t->state == TCP_SYN_RCVD) {
        if (flags & TCP_RST) {
            t->state = TCP_CLOSED;
            return;
        }
        if ((flags & TCP_ACK) && ack == t->snd_nxt && seq == t->rcv_nxt) {
            t->state = TCP_ESTABLISHED;
#if IP_TEST_MODE
            dbg_str("[tcp] ACK -> ESTABLISHED seq=");
            dbg_dec(seq);
            dbg_str(" ack=");
            dbg_dec(ack);
            dbg_str("\n");
#endif
        }
        return;
    }

    /* ESTABLISHED：数据/关闭不在本阶段处理。 */
}
