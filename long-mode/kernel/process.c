#include <stddef.h>
#include "process.h"
#include "syscall.h"
#include "exec.h"
#include "paging.h"
#include "aout.h"
#include "lib.h"

struct process proc_table[MAX_PROCS];
struct process *current;
int need_resched;

/* quantum: number of timer ticks before preemption */
#define SCHED_QUANTUM  10
static uint32_t sched_ticks;
static uint8_t __attribute__((aligned(4096))) kstack_pool[MAX_PROCS][KSTACK_SIZE];
static uint16_t kstack_in_use;   /* bit i → kstack_pool[i] allocated */

void *kstack_alloc(int pid)
{
    if (pid < 1 || pid >= MAX_PROCS)
        return NULL;
    if (kstack_in_use & (1 << pid))
        return NULL;

    kstack_in_use |= (1 << pid);
    return kstack_pool[pid] + KSTACK_SIZE;  /* top of stack */
}

void kstack_free(int pid)
{
    if (pid >= 1 && pid < MAX_PROCS)
        kstack_in_use &= ~(1 << pid);
}

/* boot PML4 physical address — defined in boot_64.S, identity mapped */
extern uint64_t pml4[];

/* called from timer_handler to account the current process's quantum */
void sched_tick(void)
{
    if (current == NULL || current->pid == 0)
        return;

    sched_ticks--;
    if (sched_ticks == 0) {
        need_resched = 1;
        sched_ticks = SCHED_QUANTUM;
    }
}

/* pick the next runnable process and switch to it.
 * if no runnable process, returns to caller (continues current). */
void schedule(void)
{
    struct process *next = NULL;
    int start = current ? current->pid : 0;

    /* first pass: prefer user processes (pid 1..MAX_PROCS-1) */
    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (idx != 0 &&
            (proc_table[idx].state == PROC_RUNNABLE ||
             proc_table[idx].state == PROC_RUNNING)) {
            next = &proc_table[idx];
            break;
        }
    }

    /* second pass: fallback to pid0 (idle task) only if no user process */
    if (next == NULL && (proc_table[0].state == PROC_RUNNABLE ||
                          proc_table[0].state == PROC_RUNNING)) {
        serial_write("fall back to pid 0", strlen("fall back to pid 0"));
        next = &proc_table[0];
    }

    if (next == NULL)
        return;

    struct process *prev = current;
    if (prev && prev->state == PROC_RUNNING)
        prev->state = PROC_RUNNABLE;

    switch_to(next);

    /* we return on next's stack */
    current->state = PROC_RUNNING;
    sched_ticks = SCHED_QUANTUM;
}

void process_init(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_table[i].pid = -1;
        proc_table[i].state = PROC_FREE;
    }

    /* pid 0: the shell — kernel-only, never has user context */
    struct process *sh = &proc_table[0];
    sh->pid         = 0;
    sh->state       = PROC_RUNNING;
    sh->k_stack_top = 0x90000;
    sh->k_rsp       = 0x90000;
    sh->k_rbp       = 0;
    sh->cr3         = (uint64_t)pml4;

    int i;
    for (i = 0; i < 5; i++) sh->path[i] = "shell"[i];
    sh->path[i] = '\0';

    current = sh;
    sched_ticks = SCHED_QUANTUM;

    /* load task register so the CPU can find the TSS for ring
     * transitions when a user process enters via iretq */
    asm volatile("movw $0x28, %%ax; ltr %%ax" ::: "ax");
}

/* ── spawn a new process ──────────────────────────────────────────── */

#define FRAME_RET      0
#define FRAME_RIP      144
#define FRAME_CS       152
#define FRAME_RFLAGS   160
#define FRAME_RSP      168
#define FRAME_SS       176

int64_t proc_spawn(const char *name)
{
    minios_exec_t *hdr = exec_lookup(name);
    if (hdr == 0)
        return -ENOENT;

    /* find free PCB slot (pid 1..15) */
    struct process *proc = 0;
    int pid;
    for (pid = 1; pid < MAX_PROCS; pid++) {
        if (proc_table[pid].state == PROC_FREE) {
            proc = &proc_table[pid];
            break;
        }
    }
    if (proc == 0)
        return -ENOMEM;

    /* allocate kernel stack */
    void *kstack = kstack_alloc(pid);
    if (kstack == 0)
        return -ENOMEM;

    /* compute virtual addresses from a.out header.
     * user virtual space starts at 0x200000 (PD[1]) and maps to the
     * process's physical slot (slot_base). */
    #define USER_VA_BASE  0x200000ULL

    uint64_t slot_base  = SLOT_BASE(pid);
    uint64_t text_va    = hdr->a_text_addr;
    uint64_t data_va    = A_DATA_BASE(*hdr);
    uint64_t bss_va     = A_BSS_BASE(*hdr);

    /* translate a program VA to its physical address in this slot */
    uint64_t text_offs = text_va - USER_VA_BASE;
    uint64_t data_offs = data_va - USER_VA_BASE;
    uint64_t bss_offs  = bss_va  - USER_VA_BASE;

    uint64_t text_phys = slot_base + text_offs;
    uint64_t data_phys = slot_base + data_offs;
    uint64_t bss_phys  = slot_base + bss_offs;

    /* stack goes above BSS, rounded up to a 2 MB boundary */
    uint64_t bss_end    = bss_va + hdr->a_bss;
    uint64_t stack_base = (bss_end + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
    uint64_t stack_top  = stack_base + STACK_PAGES * HUGE_PAGE_SIZE;
    uint64_t stack_offs = stack_base - USER_VA_BASE;
    uint64_t stack_phys = slot_base + stack_offs;

    /* user_pages from VA range (not physical slot_base) */
    uint32_t user_pages = (uint32_t)((stack_top - USER_VA_BASE) / HUGE_PAGE_SIZE);

    /* build per-process page tables */
    uint64_t new_cr3 = pt_create(slot_base, user_pages);
    if (new_cr3 == 0) {
        kstack_free(pid);
        return -ENOMEM;
    }

    /* copy text to its physical location in the slot */
    uint8_t *src = (uint8_t *)hdr + A_HDRSIZE;
    uint8_t *td  = (uint8_t *)(uintptr_t)text_phys;
    uint8_t *dd  = (uint8_t *)(uintptr_t)data_phys;
    uint8_t *bd  = (uint8_t *)(uintptr_t)bss_phys;

    for (uint32_t i = 0; i < hdr->a_text; i++)
        td[i] = src[i];

    for (uint32_t i = 0; i < hdr->a_data; i++)
        dd[i] = src[hdr->a_text + i];

    /* zero BSS */
    for (uint32_t i = 0; i < hdr->a_bss; i++)
        bd[i] = 0;

    /* zero user stack pages */
    uint8_t *sd = (uint8_t *)(uintptr_t)stack_phys;
    uint64_t stack_sz = STACK_PAGES * HUGE_PAGE_SIZE;
    for (uint64_t i = 0; i < stack_sz; i++)
        sd[i] = 0;

    /* zero any gap between BSS end and stack base (physical) */
    uint64_t gap_phys = slot_base + (bss_end - USER_VA_BASE);
    uint8_t *gap = (uint8_t *)(uintptr_t)gap_phys;
    for (uint64_t i = 0; i < stack_base - bss_end; i++)
        gap[i] = 0;

    /* fabricate initial kernel stack frame */
    uint64_t kstk_top = (uint64_t)kstack;
    uint64_t k_rsp    = kstk_top - 184;
    uint64_t *stk     = (uint64_t *)k_rsp;

    stk[FRAME_RET / 8]    = (uint64_t)&resume_user;
    stk[FRAME_RIP / 8]    = hdr->a_entry;
    stk[FRAME_CS / 8]     = 0x1B;
    stk[FRAME_RFLAGS / 8] = 0x202;
    stk[FRAME_RSP / 8]    = stack_top;
    stk[FRAME_SS / 8]     = 0x23;

    /* fill PCB — user_start/end are virtual addresses */
    proc->pid         = pid;
    proc->state       = PROC_RUNNABLE;
    proc->k_stack_top = kstk_top;
    proc->k_rsp       = k_rsp;
    proc->cr3         = new_cr3;
    proc->k_rbp       = 0;   /* resume_user pops the fabricated rbp anyway */
    proc->user_start  = USER_VA_BASE;
    proc->user_end    = stack_top;
    proc->exit_status = 0;

    for (int i = 0; i < 128; i++) {
        proc->path[i] = name[i];
        if (name[i] == '\0')
            break;
    }

    need_resched = 1;
    return pid;
}

int proc_has_runnable(void)
{
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_RUNNABLE ||
            proc_table[i].state == PROC_RUNNING)
            return 1;
    }
    return 0;
}
