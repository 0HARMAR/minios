#ifndef MINIOS_SERIAL_H
#define MINIOS_SERIAL_H

void serial_init(void);
int  serial_present(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif /* MINIOS_SERIAL_H */
