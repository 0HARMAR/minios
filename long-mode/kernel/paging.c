#include "paging.h"

/* boot page tables — exported from boot_64.S, identity-mapped.
 * pd maps 0..1GB; pd2/pd3/pd4 map 0x40000000..0xFFFFFFFF (supervisor),
 * so PCI MMIO (e1000 BAR0 @ 0xF0000000) is reachable from any context. */
extern uint64_t pd[];
extern uint64_t pd2[];
extern uint64_t pd3[];
extern uint64_t pd4[];

/* page table pool: 16 processes × 3 tables (PML4+PDP+PD) × 4096 bytes */
static uint8_t __attribute__((aligned(4096))) pt_pool[16][3][4096];
static uint16_t pt_in_use;   /* bit i set → pt_pool[i] allocated */

/* convert a virtual address (identity-mapped) to its physical address.
 * in mini-os, kernel virtual == physical, so this is a no-op. */
static inline uint64_t phys(uint64_t va) { return va; }

uint64_t pt_create(uint64_t slot_base, uint32_t user_pages)
{
    int idx;

    /* find free page-table slot */
    for (idx = 0; idx < 16; idx++) {
        if (!(pt_in_use & (1 << idx)))
            break;
    }
    if (idx == 16)
        return 0;

    pt_in_use |= (1 << idx);

    /* get the three 4096-byte-aligned pages for this slot */
    uint64_t *new_pml4 = (uint64_t *)pt_pool[idx][0];
    uint64_t *new_pdp  = (uint64_t *)pt_pool[idx][1];
    uint64_t *new_pd   = (uint64_t *)pt_pool[idx][2];

    /* zero all three tables */
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
        new_pdp[i]  = 0;
        new_pd[i]   = 0;
    }

    /* PML4[0] → new PDP (with PAGE_USER so ring-3 walks pass this level) */
    new_pml4[0] = phys((uint64_t)new_pdp) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    /* PDP[0] → new PD (with PAGE_USER for same reason) */
    new_pdp[0] = phys((uint64_t)new_pd) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

    /* PDP[1..3] → shared boot pd2/pd3/pd4 (0x40000000-0xFFFFFFFF).
     * supervisor-only so ring3 cannot touch PCI MMIO. */
    new_pdp[1] = phys((uint64_t)pd2) | PAGE_PRESENT | PAGE_WRITE;
    new_pdp[2] = phys((uint64_t)pd3) | PAGE_PRESENT | PAGE_WRITE;
    new_pdp[3] = phys((uint64_t)pd4) | PAGE_PRESENT | PAGE_WRITE;

    /* PD[0] — copy kernel mapping from boot PD (supervisor-only, no PAGE_USER) */
    new_pd[0] = pd[0];

    /* PD[1..N] — user pages for text/data/bss + stack */
    uint32_t n = user_pages;
    if (n > 511) n = 511;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t page_phys = slot_base + (uint64_t)i * HUGE_PAGE_SIZE;
        new_pd[1 + i] = page_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE | PAGE_USER;
    }

    /* PD[N+1..511] — copy remaining boot identity mappings so kernel
     * code (running during syscalls) can reach all physical memory.
     * Strip PAGE_USER so ring3 cannot access arbitrary phys addrs. */
    for (uint32_t i = 1 + n; i < 512; i++) {
        new_pd[i] = pd[i] & ~PAGE_USER;
    }

    return phys((uint64_t)new_pml4);
}

void pt_destroy(uint64_t pml4_phys)
{
    for (int idx = 0; idx < 16; idx++) {
        uint64_t pool_phys = phys((uint64_t)pt_pool[idx][0]);
        if (pool_phys == pml4_phys) {
            pt_in_use &= ~(1 << idx);
            return;
        }
    }
}
