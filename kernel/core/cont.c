/*
 * Thread continuations (ADR 0010 SC16, revision 3e). See <tarka/cont.h>.
 */

#include <tarka/cont.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>
#include <tarka/syscall.h>
#include <tarka/untyped.h>

static void cont_clear(struct thread *t)
{
    memset(&t->cont, 0, sizeof(t->cont));
}

/* One chunk of t's operation, in t's current cspace (from its binding:
 * a thread with a continuation always has one, since losing it stops
 * the thread through cont_abandon first). */
static int64_t chunk(struct thread *t)
{
    struct cspace cs;

    KASSERT(cnode_view_cap(&t->bind[TS_CSPACE], &cs) == T_OK);
    switch (t->cont.kind) {
    case CONT_REVOKE:
        return cap_revoke_chunk(&cs, &t->cont);
    default:
        panic("cont: kind %u", t->cont.kind);
    }
}

static void yield(struct thread *t)
{
    t->state = THREAD_RUNNABLE;
    thread_enqueue(t);
}

int64_t cont_after_first(struct thread *t, int64_t rc)
{
    if (t->state != THREAD_RUNNING) {
        /* The operation stopped its own thread (it deleted its binding or
         * its thread): cont_abandon has already run. */
        KASSERT(t->cont.kind == CONT_NONE);
        return rc == CONT_PENDING ? SYS_NORESULT : rc;
    }
    if (rc == CONT_PENDING) {
        KASSERT(t->cont.kind != CONT_NONE);
        yield(t);
        return SYS_NORESULT;
    }
    cont_clear(t);
    return rc;
}

#ifdef TARKA_SELFTEST
uint64_t cont_chunks_run;   /* chunks run at dispatch (tests) */
#endif

void cont_run(struct thread *t)
{
    KASSERT(t->state == THREAD_RUNNING && t->cont.kind != CONT_NONE);
#ifdef TARKA_SELFTEST
    cont_chunks_run++;
#endif
    int64_t rc = chunk(t);

    if (t->state != THREAD_RUNNING) {
        KASSERT(t->cont.kind == CONT_NONE);   /* stopped by its own chunk */
        return;
    }
    if (rc == CONT_PENDING) {
        yield(t);   /* a scheduling boundary before the next chunk */
        return;
    }
    cont_clear(t);
    if (rc != SYS_NORESULT)
        t->regs.rax = (uint64_t)rc;   /* only the final chunk writes a result */
}

void cont_abandon(struct thread *t, bool destroying)
{
    struct cspace cs;

    if (t->cont.kind == CONT_NONE)
        return;
    /* F6: release what it holds, found by identity, never by pointer. The
     * cspace binding is still intact here (the binding hook and the
     * finaliser call thread_stop before it goes). */
    if (t->cont.kind == CONT_REVOKE && cnode_view_cap(&t->bind[TS_CSPACE], &cs) == T_OK)
        cap_revoke_abandon(&cs, &t->cont);
    cont_clear(t);
    if (!destroying)
        t->regs.rax = (uint64_t)(int64_t)T_EINTR;   /* SC16, P30 */
}
