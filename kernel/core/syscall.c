/*
 * Syscall dispatch (ADR 0008, T20-T24). Marshalling only: every syscall
 * that acts on an object resolves a capability in the caller's own cspace
 * and calls the same kernel API the tests exercise (T23).
 */

#include <tarka/syscall.h>

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/selftest.h>

#include "../arch/x86_64/percpu.h"

struct sysargs {
    uint64_t a[6];   /* RDI, RSI, RDX, R10, R8, R9 */
};

typedef int64_t (*sys_fn)(struct thread *t, const struct sysargs *a);

static int64_t sys_yield(struct thread *t, const struct sysargs *a)
{
    (void)a;
    t->state = THREAD_RUNNABLE;
    thread_enqueue(t);
    return T_OK;
}

static int64_t sys_exit(struct thread *t, const struct sysargs *a)
{
    (void)a;
    t->state = THREAD_INACTIVE;
    return T_OK;
}

static const sys_fn table[] = {
    [SYS_YIELD] = sys_yield,
    [SYS_EXIT] = sys_exit,
};
#define NSYS (sizeof(table) / sizeof(table[0]))

/* T22: after the bounds check, clamp the index without a branch so a
 * mispredicted check cannot index past the table speculatively. */
static inline uint64_t index_nospec(uint64_t idx, uint64_t size)
{
    uint64_t mask;
    __asm__("cmp %2, %1; sbb %0, %0" : "=r"(mask) : "r"(idx), "r"(size) : "cc");
    return idx & mask;
}

#ifdef TARKA_SELFTEST
struct test_report test_reports[TEST_REPORT_MAX];
unsigned test_report_count;
#endif

void syscall_handle(struct thread *t)
{
    /* T21: one copy of the arguments, taken from the saved registers. */
    const uint64_t nr = t->regs.rax;
    const struct sysargs args = { { t->regs.rdi, t->regs.rsi, t->regs.rdx,
                                    t->regs.r10, t->regs.r8, t->regs.r9 } };
    int64_t rv;

#ifdef TARKA_SELFTEST
    if (nr == SYS_TEST_REPORT) {
        if (test_report_count < TEST_REPORT_MAX)
            test_reports[test_report_count++] = (struct test_report){
                args.a[0], args.a[1], args.a[2], args.a[3],
                entry_obs.last_rflags, entry_obs.last_user_rflags };
        t->regs.rax = T_OK;
        return;
    }
#endif
    if (nr >= NSYS)
        rv = T_EINVAL;   /* T31: an error, not a fault */
    else
        rv = table[index_nospec(nr, NSYS)](t, &args);

    /* The syscall may have destroyed the caller: write the result only if
     * it is still the current thread. */
    if (this_cpu()->current == t)
        t->regs.rax = (uint64_t)rv;
}
