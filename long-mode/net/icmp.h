#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>

/* 处理一个已经通过 IP 层校验的 ICMP 报文。
 * ip 指向 IP 头，ihl 是 IP 头长度（字节），total 是 IP 总长度。 */
void icmp_input(uint8_t *ip, uint16_t ihl, uint16_t total);

#endif
