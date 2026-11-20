/*
 * serial.c - polled COM1 output, used by both the loader and the kernel.
 *
 * It is intentionally tiny: the boot logs are also mirrored here so a QEMU
 * run can be checked without looking at the screen.  A missing UART is not an
 * error; the transmit-ready wait is bounded.
 */
#include "serial.h"

#define COM1 0x3F8

static int g_serial_ok;

int serial_present(void)
{
    return g_serial_ok;
}

static inline void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    outb(0x80, 0x00);
}

void serial_init(void)
{
    unsigned char probe;

    outb(COM1 + 1, 0x00);   /* disable interrupts                  */
    outb(COM1 + 3, 0x80);   /* enable DLAB                         */
    outb(COM1 + 0, 0x01);   /* divisor low: 115200 baud            */
    outb(COM1 + 1, 0x00);   /* divisor high                        */
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, one stop    */
    outb(COM1 + 2, 0xC7);   /* enable and clear FIFO               */
    outb(COM1 + 4, 0x03);   /* DTR + RTS                           */

    /*
     * Laptops usually have no COM1.  A transmit-ready loop on a floating
     * port would stall the boot log badly, so probe with the UART loopback
     * mode first and disable serial output if nothing is there.
     */
    outb(COM1 + 4, 0x1F);   /* loopback, OUT2/OUT1, RTS, DTR       */
    outb(COM1 + 0, 0xAE);
    io_wait();
    probe = inb(COM1 + 0);
    outb(COM1 + 4, 0x03);   /* back to normal operation            */
    g_serial_ok = (probe == 0xAE);
}

static int tx_ready(void)
{
    int i;

    for (i = 0; i < 100000; i++) {
        if ((inb(COM1 + 5) & 0x20) != 0)
            return 1;
    }
    return 0;
}

void serial_putc(char c)
{
    if (!g_serial_ok)
        return;
    if (c == '\n')
        serial_putc('\r');
    if (!tx_ready())
        return;
    outb(COM1, (unsigned char)c);
}

void serial_puts(const char *s)
{
    while (*s != '\0')
        serial_putc(*s++);
}
