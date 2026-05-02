#include "timer.h"
#include "io.h"
#include "irq.h"
#include "pic.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_FREQ    1193182
#define TIMER_HZ    100

static volatile uint64_t ticks = 0;

static void timer_handler(struct registers *r) {
    (void)r;
    ticks++;
}

void timer_init(void) {
    uint16_t divisor = PIT_FREQ / TIMER_HZ;

    irq_register_handler(0, timer_handler);
    pic_enable_irq(0);

    outb(PIT_CMD, 0x34);                               /* channel 0, lobyte/hibyte, rate generator, binary */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));           /* divisor low byte */
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));    /* divisor high byte */
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

void timer_wait(uint64_t n) {
    uint64_t start = ticks;
    while (ticks - start < n);
}
