/* e1000 (82540EM) NIC driver — QEMU, register access via MMIO BAR0
 * (0xF0000000, now mapped by boot_64.S's 4GB identity map).
 * TX: polling TDH/TDT. RX: IRQ-driven, frames copied to a kernel pool.
 * NOTE: QEMU's e1000 I/O BAR is a stub (reads return 0), MMIO is required.
 * See DESIGN_E1000.md. */
#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "lib.h"
#include "vga.h"

extern uint32_t vga_cursor_pos;

/* ── registers (I/O offset == MMIO offset) ───────────────────── */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

/* CTRL bits */
#define CTRL_RST     (1 << 26)
#define CTRL_SLU     (1 << 6)

/* RCTL bits */
#define RCTL_EN      (1 << 1)
#define RCTL_UPE     (1 << 3)
#define RCTL_BAM     (1 << 15)
#define RCTL_SECRC   (1 << 26)

/* TCTL bits (E1000_TCTL_EN is bit 1 on the 82540EM and in QEMU!) */
#define TCTL_EN      (1 << 1)
#define TCTL_PSP     (1 << 3)

/* desc status bits */
#define DESC_DD      (1 << 0)
#define DESC_EOP     (1 << 0)   /* in tx cmd */

/* tx cmd bits */
#define TX_CMD_EOP   (1 << 0)
#define TX_CMD_IFCS  (1 << 1)
#define TX_CMD_RS    (1 << 3)

/* EERD bits (match real 82540EM + QEMU: START=bit0, DONE=bit4, data 31:16) */
#define EERD_START   (1 << 0)
#define EERD_DONE    (1 << 4)
#define EERD_DATA    (0xFFFF)
#define EERD_ADDR_SHIFT 8

/* IMS/ICR bits */
#define ICR_RXT0     (1 << 7)
#define ICR_RXDMT0   (1 << 2)

#define RX_DESC_N    32
#define TX_DESC_N    16
#define BUF_SIZE     2048
#define RX_POOL_N    64

/* .netbuf: NOLOAD section after _kernel_end (above the kernel stack),
 * excluded from .bss so exec_init's boot-time zeroing never touches it. */
#define NETBUF __attribute__((section(".netbuf")))

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static struct rx_desc NETBUF __attribute__((aligned(16))) rx_ring[RX_DESC_N];
static struct tx_desc NETBUF __attribute__((aligned(16))) tx_ring[TX_DESC_N];
static uint8_t NETBUF rx_bufs[RX_DESC_N][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t NETBUF tx_bufs[TX_DESC_N][BUF_SIZE] __attribute__((aligned(16)));

static volatile uint32_t *e1000_mmio;
static uint8_t  e1000_mac[6];
static int32_t  e1000_ready;

/* rx_pool: kernel-side frame queue filled by IRQ handler */
static uint8_t NETBUF rx_pool[RX_POOL_N][BUF_SIZE];
static uint16_t NETBUF rx_pool_len[RX_POOL_N];
static volatile uint32_t rx_pool_head, rx_pool_tail;

static uint16_t tx_head_idx;   /* next free tx slot */
static uint16_t rx_head_idx;   /* next rx slot to reap */

static inline uint32_t e1000_read(uint32_t off) {
    return e1000_mmio[off >> 2];
}

static inline void e1000_write(uint32_t off, uint32_t val) {
    e1000_mmio[off >> 2] = val;
}

/* ── MAC via EEPROM (EERD) ──────────────────────────────────── */
static void e1000_read_mac(void) {
    for (int i = 0; i < 3; i++) {
        e1000_write(REG_EERD, EERD_START | ((uint32_t)i << EERD_ADDR_SHIFT));
        uint32_t val;
        for (int spin = 0; spin < 100000; spin++) {
            val = e1000_read(REG_EERD);
            if (val & EERD_DONE)
                break;
        }
        uint16_t word = (uint16_t)((val >> 16) & EERD_DATA);
        /* EEPROM word = (mac[2i+1] << 8) | mac[2i] */
        e1000_mac[i * 2]     = (uint8_t)(word & 0xFF);
        e1000_mac[i * 2 + 1] = (uint8_t)(word >> 8);
    }
}

/* ── RX setup ───────────────────────────────────────────────── */
static void e1000_rx_init(void) {
    for (int i = 0; i < RX_DESC_N; i++) {
        rx_ring[i].addr   = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    memset((void *)rx_pool, 0, sizeof(rx_pool));
    memset((void *)rx_pool_len, 0, sizeof(rx_pool_len));
    e1000_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    e1000_write(REG_RDBAH, (uint32_t)((uintptr_t)rx_ring >> 32));
    e1000_write(REG_RDLEN, RX_DESC_N * 16);
    e1000_write(REG_RDH, 0);
    e1000_write(REG_RDT, RX_DESC_N - 1);
    rx_head_idx = 0;
    rx_pool_head = rx_pool_tail = 0;
}

/* ── TX setup ───────────────────────────────────────────────── */
static void e1000_tx_init(void) {
    for (int i = 0; i < TX_DESC_N; i++) {
        tx_ring[i].addr   = (uint64_t)(uintptr_t)tx_bufs[i];
        tx_ring[i].status = DESC_DD;   /* all free */
    }
    e1000_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    e1000_write(REG_TDBAH, (uint32_t)((uintptr_t)tx_ring >> 32));
    e1000_write(REG_TDLEN, TX_DESC_N * 16);
    e1000_write(REG_TDH, 0);
    e1000_write(REG_TDT, 0);
    tx_head_idx = 0;
}

/* ── IRQ handler ────────────────────────────────────────────── */
static void e1000_irq_handler(struct registers *r) {
    (void)r;
    uint32_t cause = e1000_read(REG_ICR);

    if (cause & (ICR_RXT0 | ICR_RXDMT0)) {
        while (rx_ring[rx_head_idx].status & DESC_DD) {
            uint16_t i = rx_head_idx;
            uint16_t len = rx_ring[i].length;

            uint32_t next_head = (rx_pool_head + 1) % RX_POOL_N;
            if (next_head != rx_pool_tail) {
                if (len > 0 && len <= BUF_SIZE) {
                    /* RCTL.SECRC set → QEMU strips CRC, length is payload */
                    memcpy(rx_pool[rx_pool_head], rx_bufs[i], len);
                    rx_pool_len[rx_pool_head] = len;
                    rx_pool_head = next_head;
                }
            }

            rx_ring[i].status = 0;
            e1000_write(REG_RDT, i);   /* give the desc back */
            rx_head_idx = (i + 1) % RX_DESC_N;
        }
    }
}

/* ── init ───────────────────────────────────────────────────── */
void e1000_init(void) {
    struct pci_device dev;
    if (!pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev)) {
        print_string((unsigned char *)VGA_BUFFER, "[no e1000]", &vga_cursor_pos);
        print_newline((unsigned char *)VGA_BUFFER, &vga_cursor_pos);
        return;
    }

    if (dev.bar0 == 0 || (dev.bar0 & 1)) {   /* must be a 32-bit MMIO BAR */
        print_string((unsigned char *)VGA_BUFFER, "[e1000 no mmio bar]", &vga_cursor_pos);
        print_newline((unsigned char *)VGA_BUFFER, &vga_cursor_pos);
        return;
    }
    e1000_mmio = (volatile uint32_t *)(uintptr_t)(dev.bar0 & ~0xF);

    /* enable MEM + I/O + bus master (QEMU checks bus master before RX) */
    pci_write_config(dev.bus, dev.dev, dev.func, 0x04, 0x07);

    /* soft reset */
    e1000_write(REG_CTRL, CTRL_RST);
    for (int spin = 0; spin < 100000; spin++) {
        if (!(e1000_read(REG_CTRL) & CTRL_RST))
            break;
    }
    e1000_write(REG_CTRL, CTRL_SLU);   /* set link up */

    e1000_read_mac();

    /* program our MAC into receive filter */
    uint32_t ral = (uint32_t)e1000_mac[0]
                 | ((uint32_t)e1000_mac[1] << 8)
                 | ((uint32_t)e1000_mac[2] << 16)
                 | ((uint32_t)e1000_mac[3] << 24);
    uint32_t rah = (uint32_t)e1000_mac[4]
                 | ((uint32_t)e1000_mac[5] << 8)
                 | (1u << 31);           /* AV: address valid */
    e1000_write(REG_RAL0, ral);
    e1000_write(REG_RAH0, rah);

    e1000_rx_init();
    e1000_tx_init();

    /* enable RX/TX */
    e1000_write(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_BAM | RCTL_SECRC);
    e1000_write(REG_TCTL, TCTL_EN | TCTL_PSP);
    e1000_write(REG_TIPG, 0x0060200A);

    /* interrupts: RX timer timeout + RX desc min threshold */
    irq_register_handler(E1000_IRQ, e1000_irq_handler);
    pic_enable_irq(E1000_IRQ);
    e1000_write(REG_IMS, ICR_RXT0 | ICR_RXDMT0);

    e1000_ready = 1;

    unsigned char *vga = (unsigned char *)VGA_BUFFER;
    print_string(vga, "[e1000 ready] mac ", &vga_cursor_pos);
    for (int i = 0; i < 6; i++) {
        static const char hex[] = "0123456789abcdef";
        char b[3] = { hex[e1000_mac[i] >> 4], hex[e1000_mac[i] & 0xF], 0 };
        print_string(vga, b, &vga_cursor_pos);
        if (i < 5)
            print_string(vga, ":", &vga_cursor_pos);
    }
    print_newline(vga, &vga_cursor_pos);
    update_cursor(vga_cursor_pos);
}

uint8_t *e1000_get_mac(void) {
    return e1000_mac;
}

/* ── TX ─────────────────────────────────────────────────────── */
int32_t e1000_send(const uint8_t *frame, uint16_t len) {
    if (!e1000_ready || len > BUF_SIZE)
        return -1;

    uint16_t i = tx_head_idx;

    /* wait for this slot's previous packet to finish */
    for (int spin = 0; spin < 1000000; spin++) {
        if (tx_ring[i].status & DESC_DD)
            break;
        if (spin == 999999)
            return -1;
    }

    memcpy(tx_bufs[i], frame, len);
    tx_ring[i].length = len;
    tx_ring[i].status = 0;
    tx_ring[i].cmd    = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;   /* 0x0B */

    /* TDT = next free slot (exclusive): QEMU transmits [TDH, TDT),
     * writing TDT == TDH means "empty ring" and drops the packet. */
    e1000_write(REG_TDT, (uint32_t)((i + 1) % TX_DESC_N));
    tx_head_idx = (i + 1) % TX_DESC_N;
    return 0;
}

/* ── RX (consume from kernel pool) ───────────────────────────── */
int32_t e1000_recv_available(void) {
    return rx_pool_head != rx_pool_tail;
}

int32_t e1000_recv(uint8_t *out, uint16_t maxlen) {
    if (rx_pool_head == rx_pool_tail)
        return 0;

    uint32_t i = rx_pool_tail;
    uint16_t len = rx_pool_len[i];
    if (len > maxlen)
        len = maxlen;
    memcpy(out, rx_pool[i], len);
    rx_pool_tail = (rx_pool_tail + 1) % RX_POOL_N;
    return (int32_t)len;
}
