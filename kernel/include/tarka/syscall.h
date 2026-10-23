#ifndef TARKA_SYSCALL_H
#define TARKA_SYSCALL_H

#include <tarka/thread.h>

/* Syscall numbers (ADR 0008, T20, T24). */
enum {
    SYS_YIELD = 0,          /* ()                                  */
    SYS_EXIT,               /* ()                                  */
    SYS_CAP_IDENTIFY,       /* (slot, out: struct cap_info*)       */
    SYS_CAP_DERIVE,         /* (src, dst, rights_mask, badge)      */
    SYS_CAP_DELETE,         /* (slot)                              */
    SYS_CAP_REVOKE,         /* (slot, budget)                      */
    SYS_RETYPE,             /* (untyped, type, size_bits, dst, count) */
    SYS_VS_INSTALL,         /* (as, pt, va, depth)                 */
    SYS_VS_UNINSTALL,       /* (pt)                                */
    SYS_VS_MAP,             /* (as, frame, va, rights)             */
    SYS_VS_UNMAP,           /* (frame)                             */
    SYS_VS_PROTECT,         /* (frame, rights)                     */
    SYS_TH_BIND,            /* (thread, which, src)                */
    SYS_TH_UNBIND,          /* (thread, which)                     */
    SYS_TH_READ_REGS,       /* (thread, out: struct user_regs*)    */
    SYS_TH_WRITE_REGS,      /* (thread, in: const struct user_regs*) */
    SYS_TH_RESUME,          /* (thread)                            */
    SYS_TH_SUSPEND,         /* (thread)                            */
    SYS_TH_READ_FAULT,      /* (thread, out: struct fault_record*) */
    SYS_DEBUG_WRITE,        /* (console, buf, len)                 */
};

#define DEBUG_WRITE_MAX 256

/* A handler that blocked the caller returns this: the result is written
 * when the thread is woken (ADR 0009, P10), not now. */
#define SYS_NORESULT INT64_MIN

#ifdef TARKA_SELFTEST
/* Test builds only: report four values to the kernel test harness. */
#define SYS_TEST_REPORT 0x7F00
/* Test builds only: block the caller on test wait queue `rdi`. */
#define SYS_TEST_BLOCK  0x7F01
#define TEST_WAITQS     2
#endif

/* Handle the syscall `t` just made. Arguments are read from the copy of
 * the registers saved at entry (T21); the result goes back into t->regs.rax
 * only if t still exists afterwards. */
void syscall_handle(struct thread *t);

#endif
