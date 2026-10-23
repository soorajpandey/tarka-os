/*
 * Thread objects (ADR 0008). Page 0 holds the TCB, including four binding
 * slots: real capability slots, linked into the CDT like any other, so
 * revocation reaches whatever a thread is bound to. Page 1 holds the XSAVE
 * area.
 */

#include <tarka/thread.h>

#include "../../core/cap_internal.h"
#include "cpu.h"
#include "percpu.h"

#include <stddef.h>
#include <tarka/cont.h>
#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/reaper.h>
#include <tarka/sched.h>
#include <tarka/string.h>
#include <tarka/vspace.h>

_Static_assert(offsetof(struct thread, bind) % sizeof(struct cap) == 0, "binding slots must be slot-aligned");

/* ---- validation (T11-T15) ---- */

bool user_regs_valid(const struct user_regs *r)
{
    return r->rip < USER_TOP && r->rsp < USER_TOP &&
           r->fs_base < USER_TOP && r->gs_base < USER_TOP &&
           (r->rflags & ~(uint64_t)RFLAGS_USER_MASK) == 0;
}

/* T4: the conditions for being runnable. */
bool thread_runnable_safe(const struct thread *t)
{
    return t->bind[TS_ASPACE].type == CAP_ADDRESS_SPACE &&
           t->bind[TS_CSPACE].type == CAP_CNODE &&
           user_regs_valid(&t->regs);
}

/* Stop a thread (ADR 0010 SC6/G1: the one funnel, in sched.c). */
static void thread_stop(struct thread *t, bool destroying)
{
    KASSERT(sched_stop(t, destroying) == T_OK);
}

/* ---- object lifecycle ---- */

struct thread *thread_of(const struct cap *c)
{
    return phys_to_virt(cap_obj_paddr(c));
}

void thread_obj_init(paddr_t obj)
{
    struct thread *t = phys_to_virt(obj);

    /* Already zeroed by retype (O6): every register starts at 0. */
    for (unsigned i = 0; i < TS_COUNT; i++)
        t->bind[i].slot_flags = SLOT_BINDING | i;
    sched_thread_init(t);                  /* ADR 0010 SC3: its home CPU */
    fpu_init_area(thread_xsave_area(t));   /* T7 */
#ifdef TARKA_SELFTEST
    thread_registry_add(t);
#endif
}

/* T2: a required binding is about to disappear. */
static void binding_lost(struct cap *c)
{
    unsigned idx = c->slot_flags & SLOT_BINDING_IDX;
    struct thread *t = (struct thread *)((uint8_t *)(c - idx) - offsetof(struct thread, bind));

    if ((idx == TS_ASPACE || idx == TS_CSPACE) && t->state != THREAD_INACTIVE)
        thread_stop(t, false);
    /* P6a: only a receiving thread needs its IPC buffer while blocked. */
    else if (idx == TS_IPCBUF && t->state == THREAD_BLOCKED_RECV)
        thread_stop(t, false);
}

#ifdef TARKA_SELFTEST
uint64_t thread_teardowns, thread_teardowns_while_current;
#endif

/* Everything of a stopped thread's TCB that goes away with it. */
static void tcb_teardown(struct thread *t)
{
#ifdef TARKA_SELFTEST
    thread_teardowns++;
    if (sched_current() == t)
        thread_teardowns_while_current++;   /* G3: must never happen */
    thread_registry_remove(t);
#endif
    for (unsigned i = 0; i < TS_COUNT; i++)
        cap_slot_delete(&t->bind[i]);
#ifdef TARKA_SELFTEST
    /* Poison the dead register block so tests can prove nothing writes to
     * a destroyed thread afterwards (e.g. a syscall result). */
    memset(&t->regs, 0xDD, sizeof(t->regs));
#endif
}

/* T5: last Thread capability deleted. */
static void thread_finalise(enum cap_type type, uint64_t object)
{
    struct thread *t = phys_to_virt(pa(object & ~CAP_OBJ_BITS_MASK));

    (void)type;
    /* G3: stop it, and if it is destroying itself hand `current` to the
     * idle thread, before anything of it is torn down. */
    thread_stop(t, true);
    sched_release_current(t);
    tcb_teardown(t);
}

void thread_boot(void)
{
    cap_set_binding_hook(binding_lost);
    cap_set_finaliser(CAP_THREAD, thread_finalise);
}

/* ---- capability operations (T3: everything goes through the cap) ---- */

static int lookup(struct cspace *cs, cap_slot_t thr, cap_rights_t need, struct thread **out)
{
    const struct cap *c;
    int rc = cap_lookup(cs, thr, CAP_THREAD, need, &c);

    if (rc)
        return rc;
    *out = thread_of(c);
    return T_OK;
}

int thread_bind(struct cspace *cs, cap_slot_t thr, unsigned which,
                struct cspace *src_cs, cap_slot_t src)
{
    static const enum cap_type want[TS_COUNT] = {
        [TS_ASPACE] = CAP_ADDRESS_SPACE,
        [TS_CSPACE] = CAP_CNODE,
        [TS_IPCBUF] = CAP_FRAME,
        [TS_FAULT] = CAP_ENDPOINT,
    };
    struct thread *t;
    const struct cap *s;
    int rc;

    if ((rc = lookup(cs, thr, CAP_R_WRITE, &t)))
        return rc;
    if (which >= TS_COUNT)
        return T_EINVAL;
    /* T1: binding is derivation, so it needs GRANT on the source. */
    if ((rc = cap_lookup(src_cs, src, want[which], CAP_R_GRANT, &s)))
        return rc;
    if (t->bind[which].type != CAP_NULL)
        return T_EBUSY;
    /* ADR 0009 P5: the kernel writes into an IPC buffer, so binding one is
     * a writable alias: it needs READ and WRITE, and the frame must not be
     * mapped executable anywhere. */
    if (which == TS_IPCBUF) {
        if ((s->rights & (CAP_R_READ | CAP_R_WRITE)) != (CAP_R_READ | CAP_R_WRITE))
            return T_EPERM;
        if (vspace_frame_alias_conflict(s, CAP_R_WRITE))
            return T_EPERM;
    }
    return cap_derive_into(cap_slot(src_cs, src), &t->bind[which], CAP_R_ALL);
}

int thread_unbind(struct cspace *cs, cap_slot_t thr, unsigned which)
{
    struct thread *t;
    int rc;

    if ((rc = lookup(cs, thr, CAP_R_WRITE, &t)))
        return rc;
    if (which >= TS_COUNT)
        return T_EINVAL;
    if (t->bind[which].type == CAP_NULL)
        return T_EBADCAP;
    cap_slot_delete(&t->bind[which]);   /* T2 hook stops the thread if needed */
    return T_OK;
}

int thread_read_regs(struct cspace *cs, cap_slot_t thr, struct user_regs *out)
{
    struct thread *t;
    int rc = lookup(cs, thr, CAP_R_READ, &t);

    if (rc)
        return rc;
    *out = t->regs;
    return T_OK;
}

int thread_write_regs(struct cspace *cs, cap_slot_t thr, const struct user_regs *r)
{
    struct thread *t;
    struct user_regs copy = *r;   /* T21: one read of the input, then use the copy */
    int rc;

    if ((rc = lookup(cs, thr, CAP_R_WRITE, &t)))
        return rc;
    if (!user_regs_valid(&copy))
        return T_EINVAL;            /* T11-T15: nothing changes */
    t->regs = copy;
    t->ret_path = RETURN_IRET;      /* T18: externally written state never SYSRETs */
    return T_OK;
}

int thread_resume(struct cspace *cs, cap_slot_t thr)
{
    struct thread *t;
    int rc;

    if ((rc = lookup(cs, thr, CAP_R_WRITE, &t)))
        return rc;
    /* Only a stopped thread can be started. A running, runnable or
     * blocked one is already started; making a blocked one runnable would
     * leave it in its queue or reply link (ADR 0009, P9). */
    if (t->state != THREAD_INACTIVE && t->state != THREAD_FAULTED)
        return T_EBUSY;
    if (!thread_runnable_safe(t))
        return T_EPERM;             /* T4 */
    memset(&t->fault, 0, sizeof(t->fault));   /* ADR 0009 P73 */
    t->ret_path = RETURN_IRET;
    KASSERT(sched_start(t) == T_OK);   /* SC8: checked above */
    return T_OK;
}

int thread_suspend(struct cspace *cs, cap_slot_t thr)
{
    struct thread *t;
    int rc;

    if ((rc = lookup(cs, thr, CAP_R_WRITE, &t)))
        return rc;
    thread_stop(t, false);
    return T_OK;
}

int thread_read_fault(struct cspace *cs, cap_slot_t thr, struct fault_record *out)
{
    struct thread *t;
    int rc = lookup(cs, thr, CAP_R_READ, &t);

    if (rc)
        return rc;
    *out = t->fault;
    return T_OK;
}

int thread_get_state(struct cspace *cs, cap_slot_t thr, uint32_t *out)
{
    struct thread *t;
    int rc = lookup(cs, thr, CAP_R_READ, &t);

    if (rc)
        return rc;
    *out = t->state;
    return T_OK;
}
