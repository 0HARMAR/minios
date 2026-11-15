#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_MSS     1460
#define TCP_WINDOW  4096

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
};

struct tcp_tcb {
    enum tcp_state state;

    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    /* 发送侧（握手只需要 iss / snd_una / snd_nxt） */
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint16_t snd_wnd;
    uint16_t mss;

    /* 接收侧 */
    uint32_t irs;
    uint32_t rcv_nxt;
    uint16_t rcv_wnd;
};

/* 在 port 上建立 LISTEN TCB；成功返回 0。 */
int tcp_listen(uint16_t port);

/* 由 ip_handle() 在 proto == 6 时调用。 */
void tcp_input(uint8_t *ip, uint16_t ihl, uint16_t total);

#endif
