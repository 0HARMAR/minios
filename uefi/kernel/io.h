#ifndef MINIOS_IO_H
#define MINIOS_IO_H

/* Minimal x86 port I/O for the freestanding kernel. */
static inline void outl(unsigned short port, unsigned int value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int value;

    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(unsigned short port, unsigned short value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short value;

    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif /* MINIOS_IO_H */
