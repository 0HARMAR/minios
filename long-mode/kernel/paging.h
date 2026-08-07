#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* page flags */
#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_HUGE       (1 << 7)

/* per-process address-space slot */
#define SLOT_SIZE       (30ULL * 0x200000)   /* 60 MB (30 × 2 MB huge pages) */
#define SLOT_BASE(pid)  (0x200000ULL + (uint64_t)((pid) - 1) * SLOT_SIZE)
#define STACK_PAGES     1                     /* one 2 MB page for user stack */

/* page table entry helpers */
#define PT_ENTRY(addr, flags)  ((uint64_t)(addr) | (uint64_t)(flags))

/* 2 MB per huge page; 512 entries per page table */
#define HUGE_PAGE_SIZE  0x200000ULL
#define PT_ENTRIES      512

/* build per-process page tables.  user_pages = number of 2 MB user pages
 * (text+data+bss rounded up, plus stack).  returns physical address of
 * the new PML4, or 0 if no free page-table slot. */
uint64_t pt_create(uint64_t slot_base, uint32_t user_pages);

/* free page tables back to the pool */
void pt_destroy(uint64_t pml4_phys);

#endif
