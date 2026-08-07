#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define MAX_PROCS      16
#define PATH_MAX       128
#define KSTACK_SIZE    4096      /* one 4 KB page per kernel stack */

enum proc_state {
    PROC_FREE = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_ZOMBIE,
};

struct process {
    /* identity */
    int32_t       pid;
    char          path[PATH_MAX];
    enum proc_state state;

    /* kernel context — k_rsp anchors the full user CPU state (all GPRs
     * plus the iretq frame) on this process's kernel stack.  k_rbp keeps
     * the frame pointer so a cross-process resume can unwind its own
     * schedule() frame instead of the previous process's. */
    uint64_t      k_rsp;
    uint64_t      k_stack_top;

    /* memory */
    uint64_t      cr3;          /* physical address of PML4 */
    uint64_t      k_rbp;        /* saved RBP at switch time */
    uint64_t      user_start;   /* lowest valid user virtual address */
    uint64_t      user_end;     /* highest valid user virtual address + 1 */

    /* exit */
    int64_t       exit_status;
};

extern struct process proc_table[MAX_PROCS];
extern struct process *current;
extern int need_resched;

void process_init(void);
void schedule(void);
void sched_tick(void);
void switch_to(struct process *next);
void resume_user(void);

/* kernel stack pool — allocate/free for user processes (pid 1..15).
 * pid 0 (shell) uses the boot stack at 0x90000. */
void *kstack_alloc(int pid);
void  kstack_free(int pid);

/* spawn a process from the built-in program table.  returns pid,
 * or a negative error code.  called by both the syscall path
 * (sys_crtproc) and the kernel shell (ush command). */
int64_t proc_spawn(const char *name);

/* true while any user process (pid != 0) is RUNNABLE or RUNNING */
int proc_has_runnable(void);

#endif
