#ifndef E1000_H
#define E1000_H

#include <stdint.h>

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E
#define E1000_IRQ    11

/* one-shot init + enable. prints status via VGA. */
void e1000_init(void);

/* send one ethernet frame (dst mac + src mac + ethertype + payload). returns 0 on ok. */
int32_t e1000_send(const uint8_t *frame, uint16_t len);

/* non-blocking receive: 0 if no frame, 1 if copied to out (len-4, CRC stripped) */
int32_t e1000_recv(uint8_t *out, uint16_t maxlen);
int32_t e1000_recv_available(void);

uint8_t *e1000_get_mac(void);

#endif
