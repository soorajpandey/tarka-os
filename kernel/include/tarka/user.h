#ifndef TARKA_USER_H
#define TARKA_USER_H

#include <tarka/thread.h>

/* User entry, return, scheduling (ADR 0008). */

#define VEC_SYSCALL   0x100  /* trap_frame vector for SYSCALL entries */
#define VEC_BAD_STATE 0x101  /* fault record: invalid state on return (T17) */

/* SYSCALL MSRs, legacy PIC masked, IDT gate privileges. */
void user_boot(void);

/* Run user threads until none is runnable, then return (kernel context:
 * boot path and tests). Not reentrant. */
void sched_run(void);

#endif
