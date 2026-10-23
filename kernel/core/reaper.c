/*
 * The reaper (ADR 0010, SC23, SC24, R0-R9; revision 3d D2, D4, D5): the
 * kernel-owned teardown of objects that cannot die in O(1). See
 * <tarka/reaper.h>.
 *
 * The list is singly linked through each listed object's own reaper link
 * (in the object for an endpoint or notification, in the CNode header for
 * a CNode). A list entry is a pointer to the object's struct obj_meta,
 * tagged in its low 2 bits with the kind of object. Listed objects are
 * pinned internal references (O10): a dying object's memory cannot be
 * reused until its completing unit has run (R4), and a live doomed one is
 * unlisted before it can die.
 */

#include <tarka/reaper.h>

#include "cap_internal.h"

#include <tarka/endpoint.h>
#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/notification.h>
#include <tarka/string.h>
#include <tarka/untyped.h>

#define KIND_MASK 0x3ull
#define KIND_EP    1
#define KIND_NTFN  2
#define KIND_CNODE 3

_Static_assert(_Alignof(struct obj_meta) >= 4, "the reaper tags obj_meta pointers");

static uint64_t head, tail;   /* tagged; 0 when empty */

#ifdef TARKA_SELFTEST
struct reap_event reap_trace[REAP_TRACE_MAX];
unsigned reap_trace_n;

static void trace(const struct obj_meta *m, unsigned kind, enum reap_act act)
{
    if (reap_trace_n < REAP_TRACE_MAX)
        reap_trace[reap_trace_n++] = (struct reap_event){ m->serial, act, kind };
}
#else
#define trace(m, kind, act) ((void)0)
#endif

static unsigned kind_of(uint64_t x) { return (unsigned)(x & KIND_MASK); }
static struct obj_meta *meta_of(uint64_t x) { return (struct obj_meta *)(uintptr_t)(x & ~KIND_MASK); }

static struct endpoint *ep_of(struct obj_meta *m)
{
    return (struct endpoint *)((uint8_t *)m - offsetof(struct endpoint, meta));
}

static struct notification *ntfn_of(struct obj_meta *m)
{
    return (struct notification *)((uint8_t *)m - offsetof(struct notification, meta));
}

/* A CNode's metadata is slot 0's slot_meta (SC19a). */
static struct cap *cnode_slots_of(struct obj_meta *m)
{
    return (struct cap *)((uint8_t *)m - offsetof(struct cap, slot_meta));
}

static uint64_t *link_of(uint64_t x)
{
    struct obj_meta *m = meta_of(x);

    switch (kind_of(x)) {
    case KIND_EP:
        return &ep_of(m)->reap_next;
    case KIND_NTFN:
        return &ntfn_of(m)->reap_next;
    default:
        return &m->reap_next;
    }
}

static void list_append(struct obj_meta *m, unsigned kind)
{
    uint64_t x = (uint64_t)(uintptr_t)m | kind;

    /* R2: listed at most once; the link is set exactly while listed. */
    KASSERT(!(m->flags & OBJ_LISTED) && *link_of(x) == 0);
    if (tail)
        *link_of(tail) = x;
    else
        head = x;
    tail = x;
    m->flags |= OBJ_LISTED;
}

static void list_pop(void)
{
    uint64_t x = head;
    uint64_t *link = link_of(x);

    head = *link;
    if (!head)
        tail = 0;
    *link = 0;
    meta_of(x)->flags &= ~OBJ_LISTED;
}

static unsigned kind_for(uint32_t type)
{
    switch (type) {
    case CAP_ENDPOINT:
        return KIND_EP;
    case CAP_NOTIFICATION:
        return KIND_NTFN;
    case CAP_CNODE:
        return KIND_CNODE;
    default:
        return 0;
    }
}

static bool has_waiters(const struct cap *c)
{
    if (c->type == CAP_ENDPOINT) {
        const struct endpoint *e = endpoint_of(c);
        return e->senders.head || e->receivers.head;
    }
    return notification_of(c)->waiters.head != NULL;
}

bool reaper_take(struct cap *c, struct obj_meta *m)
{
    unsigned kind = kind_for(c->type);

    if (!kind)
        return false;   /* finalised at once, within UNIT_MAX (D4) */
    KASSERT(m && m->live == 0 && !(m->flags & OBJ_DYING));
    /* Every CNode goes to the reaper (its slots may hold anything); an
     * endpoint or notification only if something is left to do. */
    if (kind != KIND_CNODE && !(m->flags & OBJ_LISTED) && !has_waiters(c))
        return false;

    /* R0, in this order and in one step: dying, carver captured, its
     * dying count raised (the memory is now pinned), listed. The caller
     * then removes the last record. */
    struct cap *carver = m->carver;
    KASSERT(carver && carver->type == CAP_UNTYPED && carver->ut.dying < UINT32_MAX);
    m->flags |= OBJ_DYING;
    carver->ut.dying++;
    if (!(m->flags & OBJ_LISTED))
        list_append(m, kind);   /* a doomed object is already listed (R2) */
    return true;
}

void reaper_doom(const struct cap *c, struct obj_meta *m, uint32_t doom)
{
    KASSERT(doom == OBJ_DOOM_SEND || doom == OBJ_DOOM_RECV);
    KASSERT(!(m->flags & OBJ_DYING) && m->live > 0);
    m->flags |= doom;
    if (!(m->flags & OBJ_LISTED))
        list_append(m, kind_for(c->type));
}

/* Teardown complete (R0's second transaction, in one unit): off the list,
 * and the dying count drops. Only now may the memory be reused. */
static void finish(struct obj_meta *m)
{
    struct cap *carver = m->carver;

    KASSERT((m->flags & OBJ_DYING) && carver->ut.dying > 0);
    list_pop();
    carver->ut.dying--;
    m->flags &= ~(OBJ_DYING | OBJ_DOOM_SEND | OBJ_DOOM_RECV);
    meta_retire(m);
}

#ifdef TARKA_SELFTEST
uint64_t reaper_units_total;
#endif

/* One unit of work on the head of the list (SC24). */
static void unit(void)
{
#ifdef TARKA_SELFTEST
    reaper_units_total++;
#endif
    uint64_t x = head;
    unsigned kind = kind_of(x);
    struct obj_meta *m = meta_of(x);
    bool dying = m->flags & OBJ_DYING;
    struct thread *w = NULL;

    if (kind == KIND_EP) {
        struct endpoint *e = ep_of(m);
        if (e->senders.head && (dying || (m->flags & OBJ_DOOM_SEND)))
            w = e->senders.head;
        else if (e->receivers.head && (dying || (m->flags & OBJ_DOOM_RECV)))
            w = e->receivers.head;
    } else if (kind == KIND_NTFN) {
        w = ntfn_of(m)->waiters.head;
        KASSERT(!w || dying || (m->flags & OBJ_DOOM_RECV));
    } else {
        /* A dying CNode: one slot per unit, from a cursor kept in slot 0
         * (a slot property, so deleting slot 0's record keeps it). */
        struct cap *slots = cnode_slots_of(m);
        uint32_t n = 1u << (((m->flags & OBJ_BITS_MASK) >> OBJ_BITS_SHIFT) - CAP_SLOT_BITS);
        uint32_t i = slots[0].slot_aux;

        KASSERT(dying);
        while (i < n && slots[i].type == CAP_NULL)
            i++;
        slots[0].slot_aux = i;
        if (i < n) {
            if (cap_reap_slot(&slots[i]) == REAP_SLOT_BLOCKED) {
                /* D2: requeue, never wait. Everything this waits for was
                 * created later (serial order) and is listed ahead now. */
                trace(m, kind, REAP_REQUEUE);
                list_pop();
                list_append(m, kind);
                return;
            }
            trace(m, kind, REAP_REAL);
            return;
        }
        slots[0].slot_aux = 0;
        trace(m, kind, REAP_DONE);
        finish(m);
        return;
    }

    if (w) {
        thread_wake(w, T_EDEAD);   /* exactly once: it leaves the queue */
        trace(m, kind, REAP_REAL);
        return;
    }
    trace(m, kind, REAP_DONE);
    if (dying) {
        finish(m);
    } else {
        /* A doomed live object whose doomed waiters are all gone. */
        list_pop();
        m->flags &= ~(OBJ_DOOM_SEND | OBJ_DOOM_RECV);
    }
}

unsigned reaper_run(unsigned units)
{
    unsigned done = 0;

    while (done < units && head) {
        unit();
        done++;
    }
    return done;
}

bool reaper_idle(void)
{
    return head == 0;
}

#ifdef TARKA_SELFTEST
bool reaper_list_next(uint64_t *cursor, unsigned *kind, const struct obj_meta **m)
{
    uint64_t x = *cursor ? *link_of(*cursor) : head;

    if (!x)
        return false;
    *cursor = x;
    *kind = kind_of(x);
    *m = meta_of(x);
    return true;
}

unsigned reaper_drain(unsigned long max_units)
{
    unsigned long done = 0;

    while (head) {
        if (done++ == max_units)
            panic("reaper: no end after %lu units", max_units);
        unit();
    }
    return (unsigned)done;
}
#endif
