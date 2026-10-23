/*
 * Syscall dispatch (ADR 0008, T20-T24). Marshalling only (T23): every
 * syscall that acts on an object resolves capability slots in the
 * caller's own bound cspace and calls the same kernel API the in-kernel
 * tests exercise, with the same rights checks. There is no permission
 * logic here.
 */

#include <tarka/syscall.h>

#include <tarka/cont.h>
#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/endpoint.h>
#include <tarka/kernel.h>
#include <tarka/notification.h>
#include <tarka/selftest.h>
#include <tarka/uaccess.h>
#include <tarka/untyped.h>
#include <tarka/vspace.h>

#include "cap_internal.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/serial.h"

struct sysargs {
    uint64_t a[6];   /* RDI, RSI, RDX, R10, R8, R9 */
};

struct sysctx {
    struct thread *t;
    struct cspace  cs;   /* the caller's cspace, from its binding */
};

typedef int64_t (*sys_fn)(struct sysctx *c, const struct sysargs *a);

/* Arguments are 64-bit registers. Narrower parameters are range-checked,
 * never truncated: slot 0x1_0000_0005 must not alias slot 5. */
#define SLOT(i, out)                                \
    do {                                            \
        if (a->a[i] > UINT32_MAX)                   \
            return T_ERANGE;                        \
        (out) = (cap_slot_t)a->a[i];                \
    } while (0)
#define U32(i, out)                                 \
    do {                                            \
        if (a->a[i] > UINT32_MAX)                   \
            return T_EINVAL;                        \
        (out) = (uint32_t)a->a[i];                  \
    } while (0)

static int64_t sys_yield(struct sysctx *c, const struct sysargs *a)
{
    (void)a;
    c->t->state = THREAD_RUNNABLE;
    thread_enqueue(c->t);
    return T_OK;
}

static int64_t sys_exit(struct sysctx *c, const struct sysargs *a)
{
    (void)a;
    /* ADR 0009 P64: exiting is stopping, so reply authority held dies
     * with it and its caller is woken with T_EDEAD (P28). */
    ipc_detach(c->t, false);
    c->t->state = THREAD_INACTIVE;
    return T_OK;
}

static int64_t sys_cap_identify(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;
    struct cap_info info;
    int rc;

    SLOT(0, s);
    if ((rc = cap_identify(&c->cs, s, &info)))
        return rc;
    return copy_to_user(ua(a->a[1]), &info, sizeof(info));
}

static int64_t sys_cap_derive(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t src, dst;
    uint32_t mask;

    SLOT(0, src);
    SLOT(1, dst);
    U32(2, mask);
    return cap_derive(&c->cs, src, dst, mask, a->a[3]);
}

static int64_t sys_cap_delete(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return cap_delete(&c->cs, s);
}

/* ADR 0010 SC22, revision 3e E6: runs to completion as a continuation.
 * The old budget argument is ignored: the kernel chooses the chunk. */
static int64_t sys_cap_revoke(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;
    int rc;

    SLOT(0, s);
    rc = cap_revoke_begin(&c->cs, s, &c->t->cont);
    if (rc == CONT_PENDING)
        rc = cap_revoke_chunk(&c->cs, &c->t->cont);   /* the first chunk */
    return cont_after_first(c->t, rc);
}

static int64_t sys_retype(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t ut, dst;
    uint32_t type, bits, count;

    SLOT(0, ut);
    U32(1, type);
    U32(2, bits);
    SLOT(3, dst);
    U32(4, count);
    if (type >= CAP_TYPE_COUNT)
        return T_EINVAL;
    return cap_retype(&c->cs, ut, (enum cap_type)type, bits, &c->cs, dst, count);
}

static int64_t sys_vs_install(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t as, pt;
    uint32_t depth;

    SLOT(0, as);
    SLOT(1, pt);
    U32(3, depth);
    return vspace_install(&c->cs, as, pt, ua(a->a[2]), depth);
}

static int64_t sys_vs_uninstall(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t pt;

    SLOT(0, pt);
    return vspace_uninstall(&c->cs, pt);
}

static int64_t sys_vs_map(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t as, fr;
    uint32_t rights;

    SLOT(0, as);
    SLOT(1, fr);
    U32(3, rights);
    return vspace_map(&c->cs, as, fr, ua(a->a[2]), rights);
}

static int64_t sys_vs_unmap(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t fr;

    SLOT(0, fr);
    return vspace_unmap(&c->cs, fr);
}

static int64_t sys_vs_protect(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t fr;
    uint32_t rights;

    SLOT(0, fr);
    U32(1, rights);
    return vspace_protect(&c->cs, fr, rights);
}

static int64_t sys_th_bind(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr, src;
    uint32_t which;

    SLOT(0, thr);
    U32(1, which);
    SLOT(2, src);
    return thread_bind(&c->cs, thr, which, &c->cs, src);
}

static int64_t sys_th_unbind(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;
    uint32_t which;

    SLOT(0, thr);
    U32(1, which);
    return thread_unbind(&c->cs, thr, which);
}

static int64_t sys_th_read_regs(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;
    struct user_regs r;
    int rc;

    SLOT(0, thr);
    if ((rc = thread_read_regs(&c->cs, thr, &r)))
        return rc;
    return copy_to_user(ua(a->a[1]), &r, sizeof(r));
}

static int64_t sys_th_write_regs(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;
    struct user_regs r;
    int rc;

    SLOT(0, thr);
    /* T27: one copy; everything after this uses only `r`. */
    if ((rc = copy_from_user(&r, ua(a->a[1]), sizeof(r))))
        return rc;
    return thread_write_regs(&c->cs, thr, &r);
}

static int64_t sys_th_resume(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;

    SLOT(0, thr);
    return thread_resume(&c->cs, thr);
}

static int64_t sys_th_suspend(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;

    SLOT(0, thr);
    return thread_suspend(&c->cs, thr);
}

static int64_t sys_th_read_fault(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t thr;
    struct fault_record f;
    int rc;

    SLOT(0, thr);
    if ((rc = thread_read_fault(&c->cs, thr, &f)))
        return rc;
    return copy_to_user(ua(a->a[1]), &f, sizeof(f));
}

static int64_t sys_debug_write(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t con;
    const struct cap *cap;
    char buf[DEBUG_WRITE_MAX];
    int rc;

    SLOT(0, con);
    /* T24: even the temporary console needs a capability. */
    if ((rc = cap_lookup(&c->cs, con, CAP_DEBUG_CONSOLE, CAP_R_WRITE, &cap)))
        return rc;
    if (a->a[2] > DEBUG_WRITE_MAX)
        return T_EINVAL;
    size_t len = (size_t)a->a[2];
    if ((rc = copy_from_user(buf, ua(a->a[1]), len)))
        return rc;
    /* No terminal control sequences from user space in the kernel log. */
    for (size_t i = 0; i < len; i++)
        if ((buf[i] < 0x20 && buf[i] != '\n') || buf[i] == 0x7F)
            buf[i] = '.';
    serial_write("[user] ", 7);
    serial_write(buf, len);
    return (int64_t)len;
}

static int64_t sys_signal(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return notification_signal(c->t, &c->cs, s);   /* no bits argument: P20 */
}

static int64_t sys_wait(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return notification_wait(c->t, &c->cs, s, true);
}

static int64_t sys_poll(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return notification_wait(c->t, &c->cs, s, false);
}

static int64_t sys_send(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return endpoint_send(c->t, &c->cs, s, a->a[1]);
}

static int64_t sys_recv(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return endpoint_recv(c->t, &c->cs, s);
}

static int64_t sys_call(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return endpoint_call(c->t, &c->cs, s, a->a[1]);
}

static int64_t sys_reply(struct sysctx *c, const struct sysargs *a)
{
    return endpoint_reply(c->t, &c->cs, a->a[0]);
}

static int64_t sys_reply_recv(struct sysctx *c, const struct sysargs *a)
{
    cap_slot_t s;

    SLOT(0, s);
    return endpoint_reply_recv(c->t, &c->cs, s, a->a[1]);
}

static const sys_fn table[] = {
    [SYS_YIELD] = sys_yield,
    [SYS_EXIT] = sys_exit,
    [SYS_CAP_IDENTIFY] = sys_cap_identify,
    [SYS_CAP_DERIVE] = sys_cap_derive,
    [SYS_CAP_DELETE] = sys_cap_delete,
    [SYS_CAP_REVOKE] = sys_cap_revoke,
    [SYS_RETYPE] = sys_retype,
    [SYS_VS_INSTALL] = sys_vs_install,
    [SYS_VS_UNINSTALL] = sys_vs_uninstall,
    [SYS_VS_MAP] = sys_vs_map,
    [SYS_VS_UNMAP] = sys_vs_unmap,
    [SYS_VS_PROTECT] = sys_vs_protect,
    [SYS_TH_BIND] = sys_th_bind,
    [SYS_TH_UNBIND] = sys_th_unbind,
    [SYS_TH_READ_REGS] = sys_th_read_regs,
    [SYS_TH_WRITE_REGS] = sys_th_write_regs,
    [SYS_TH_RESUME] = sys_th_resume,
    [SYS_TH_SUSPEND] = sys_th_suspend,
    [SYS_TH_READ_FAULT] = sys_th_read_fault,
    [SYS_DEBUG_WRITE] = sys_debug_write,
    [SYS_SIGNAL] = sys_signal,
    [SYS_WAIT] = sys_wait,
    [SYS_POLL] = sys_poll,
    [SYS_SEND] = sys_send,
    [SYS_RECV] = sys_recv,
    [SYS_CALL] = sys_call,
    [SYS_REPLY] = sys_reply,
    [SYS_REPLY_RECV] = sys_reply_recv,
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
struct waitq test_waitqs[TEST_WAITQS] = {
    { .kind = THREAD_BLOCKED_RECV }, { .kind = THREAD_BLOCKED_NOTIF },
};
#endif

void syscall_handle(struct thread *t)
{
    /* T21: one copy of the arguments, taken from the saved registers. */
    const uint64_t nr = t->regs.rax;
    const struct sysargs args = { { t->regs.rdi, t->regs.rsi, t->regs.rdx,
                                    t->regs.r10, t->regs.r8, t->regs.r9 } };
    struct sysctx ctx = { .t = t };
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
    if (nr == SYS_TEST_BLOCK) {
        if (args.a[0] >= TEST_WAITQS) {
            t->regs.rax = (uint64_t)(int64_t)T_EINVAL;
            return;
        }
        waitq_block(t, &test_waitqs[args.a[0]]);
        return;   /* the result arrives with the wake-up */
    }
#endif
    if (nr >= NSYS)
        rv = T_EINVAL;   /* T31: an error, not a fault */
    else if (cnode_view_cap(&t->bind[TS_CSPACE], &ctx.cs) != T_OK)
        rv = T_EBADCAP;  /* unreachable while T4 holds; defensive */
    else
        rv = table[index_nospec(nr, NSYS)](&ctx, &args);

    /* The syscall may have destroyed the caller: write the result only if
     * it is still the current thread, and only if it did not block. */
    if (this_cpu()->current == t && rv != SYS_NORESULT)
        t->regs.rax = (uint64_t)rv;
}
