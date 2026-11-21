#ifndef MINIOS_LOG_H
#define MINIOS_LOG_H

/* Kernel log: mirrored to COM1 (if present) and the GOP framebuffer. */
void klog(const char *fmt, ...);

/* Complete log text (NUL-terminated) for the optional UEFI variable dump. */
const char *klog_data(unsigned long *len);

#endif /* MINIOS_LOG_H */
