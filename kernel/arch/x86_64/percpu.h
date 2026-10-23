#ifndef TARKA_X86_64_PERCPU_H
#define TARKA_X86_64_PERCPU_H

#include <tarka/types.h>

struct thread;

/*
 * Per-CPU data, reached through the GS base while in the kernel (ADR 0008,
 * T9, T32). The assembly entry stubs use the fixed offsets below; keep
 * them in sync (checked by _Static_assert in percpu.c).
 */
struct percpu {
    struct percpu *self;          /* gs:0  */
    uint64_t       kstack_top;    /* gs:8  RSP0 for SYSCALL entry */
    uint64_t       user_rsp;      /* gs:16 scratch: user RSP at SYSCALL */
    struct thread *current;       /* gs:24 thread running on this CPU */
    uint32_t       cpu_id;        /* gs:32 */
    uint32_t       reserved;
};

#define PERCPU_SELF       0
#define PERCPU_KSTACK_TOP 8
#define PERCPU_USER_RSP   16
#define PERCPU_CURRENT    24

#define MSR_FS_BASE        0xC0000100u
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

/* Point GS at this CPU's data; the user GS base starts at 0. */
void percpu_init(void);

static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

#endif
