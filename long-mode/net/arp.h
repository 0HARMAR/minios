#ifndef ARP_H
#define ARP_H

#include <stdint.h>

void arp_init();

int arp_lookup(uint32_t ip, uint8_t mac_out[6]);

void arp_update(uint32_t ip, const uint8_t mac[6]);

/* 地址解析，命中返回1，mac_out填充mac地址。
 * 未命中广播request，返回0。
*/
int arp_resolve(uint32_t ip, uint8_t mac_out[6]);

void arp_handle(const uint8_t *frame, uint16_t len);

#endif
