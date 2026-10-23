#ifndef TARKA_SYSCALL_H
#define TARKA_SYSCALL_H

#include <tarka/thread.h>

/* Syscall numbers (ADR 0008, T20, T24). */
enum {
    SYS_YIELD = 0,
    SYS_EXIT,
    SYS_COUNT_BASE,
};

#ifdef TARKA_SELFTEST
/* Test builds only: report three values to the kernel test harness. */
#define SYS_TEST_REPORT 0x7F00
#endif

/* Handle the syscall `t` just made. Arguments are read from the copy of
 * the registers saved at entry (T21); the result goes back into t->regs.rax
 * only if t still exists afterwards. */
void syscall_handle(struct thread *t);

#endif
