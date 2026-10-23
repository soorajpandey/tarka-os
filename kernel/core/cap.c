#include <tarka/cap.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>

static cap_finaliser_t finalisers[CAP_TYPE_COUNT];
static cap_cleanup_t cleanups[CAP_TYPE_COUNT];

/* Incremented on every slot deletion. Lets revocation notice that a
 * finaliser deleted further capabilities behind its back (ADR 0006, O11). */
static uint64_t cdt_generation;

void cap_set_finaliser(enum cap_type type, cap_finaliser_t fn)
{
    KASSERT(type > CAP_NULL && type < CAP_TYPE_COUNT);
    finalisers[type] = fn;
}

void cap_set_cleanup(enum cap_type type, cap_cleanup_t fn)
{
    KASSERT(type > CAP_NULL && type < CAP_TYPE_COUNT);
    cleanups[type] = fn;
}

void cspace_init(struct cspace *cs, struct cap *storage, uint32_t nslots)
{
    memset(storage, 0, sizeof(*storage) * nslots);
    cs->slots = storage;
    cs->nslots = nslots;
}

struct cap *cap_slot(const struct cspace *cs, cap_slot_t slot)
{
    /* Slot indices will arrive straight from user registers: bounds-check
     * before every access. */
    if (slot >= cs->nslots)
        return NULL;
    return &cs->slots[slot];
}

bool cap_same_object(const struct cap *a, const struct cap *b)
{
    return a && b && a->type == b->type && a->object == b->object;
}

struct cap *cap_object_first(struct cap *c)
{
    while (cap_same_object(c->prev, c))
        c = c->prev;
    return c;
}

/*
 * All capabilities to one object are contiguous in the CDT: the kernel
 * installs one root per object and every copy is derived from it. So a
 * capability is the last one to its object exactly when neither CDT
 * neighbour refers to the same object.
 */
static bool is_final(const struct cap *c)
{
    return !cap_same_object(c->prev, c) && !cap_same_object(c->next, c);
}

/* Unlink `c` from the CDT. Its descendants move up one level so they
 * become children of `c`'s parent instead of being orphaned. */
static void cdt_remove(struct cap *c)
{
    for (struct cap *n = c->next; n && n->depth > c->depth; n = n->next)
        n->depth--;
    if (c->prev)
        c->prev->next = c->next;
    if (c->next)
        c->next->prev = c->prev;
}

bool cap_has_children(const struct cap *c)
{
    return c->next && c->next->depth > c->depth;
}

void cap_insert_child(struct cap *parent, struct cap *child)
{
    child->depth = parent->depth + 1;
    child->prev = parent;
    child->next = parent->next;
    if (parent->next)
        parent->next->prev = child;
    parent->next = child;
}

static void destroy_slot(struct cap *c)
{
    enum cap_type type = c->type;

    /* Undo per-capability state (e.g. a mapping) while the capability is
     * still intact. */
    if (cleanups[type])
        cleanups[type](c);

    bool final = is_final(c);
    uint64_t object = c->object;

    cdt_remove(c);
    cdt_generation++;
    /* Clear the slot before the finaliser runs so nothing can observe or
     * reuse a half-deleted capability. */
    memset(c, 0, sizeof(*c));

    if (final && finalisers[type])
        finalisers[type](type, object);
}

static struct cap *subtree_last(struct cap *c)
{
    struct cap *last = c;

    for (struct cap *n = c->next; n && n->depth > c->depth; n = n->next)
        last = n;
    return last;
}

static int revoke(struct cap *c, uint32_t budget)
{
    struct cap *last = subtree_last(c);

    /* Delete from the end of the subtree backwards. In pre-order every
     * node's descendants follow it, so each node is a leaf by the time it
     * is reached: nothing needs re-parenting, and stopping part-way leaves
     * a valid tree that the next call continues from. */
    for (uint32_t done = 0; last != c; done++) {
        if (budget && done == budget)
            return T_EAGAIN;

        struct cap *prev = last->prev;
        uint64_t gen = cdt_generation;

        destroy_slot(last);

        /* A finaliser (e.g. a CNode's) may have deleted more slots,
         * possibly including `prev` or even `c`. The memory is still not
         * reusable (no retype can run meanwhile), but the saved pointer
         * may now name an empty slot: re-find the end of the subtree. */
        if (c->type == CAP_NULL)
            return T_OK;
        last = cdt_generation == gen + 1 ? prev : subtree_last(c);
    }
    return T_OK;
}

void cspace_destroy(struct cspace *cs)
{
    for (uint32_t i = 0; i < cs->nslots; i++) {
        struct cap *c = &cs->slots[i];

        if (c->type == CAP_UNTYPED)
            (void)revoke(c, 0);
        if (c->type != CAP_NULL)
            destroy_slot(c);
    }
    cs->slots = NULL;
    cs->nslots = 0;
}

int cap_install(struct cspace *cs, cap_slot_t slot, enum cap_type type,
                cap_rights_t rights, uint64_t object)
{
    struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (type == CAP_NULL || type >= CAP_TYPE_COUNT || (rights & ~CAP_R_ALL))
        return T_EINVAL;
    if (c->type != CAP_NULL)
        return T_EBUSY;

    memset(c, 0, sizeof(*c));
    c->type = type;
    c->rights = rights;
    c->object = object;
    return T_OK;
}

int cap_lookup(const struct cspace *cs, cap_slot_t slot, enum cap_type type,
               cap_rights_t need, const struct cap **out)
{
    const struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL || c->type != type)
        return T_EBADCAP;
    if ((c->rights & need) != need)
        return T_EPERM;

    *out = c;
    return T_OK;
}

int cap_identify(const struct cspace *cs, cap_slot_t slot, struct cap_info *out)
{
    const struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    memset(out, 0, sizeof(*out));
    out->type = c->type;
    out->rights = c->rights;
    out->badge = c->type == CAP_ENDPOINT ? c->badge : 0;
    return T_OK;
}

int cap_derive_to(struct cspace *src_cs, cap_slot_t src,
                  struct cspace *dst_cs, cap_slot_t dst,
                  cap_rights_t mask, uint64_t badge)
{
    struct cap *s = cap_slot(src_cs, src);
    struct cap *d = cap_slot(dst_cs, dst);

    if (!s || !d)
        return T_ERANGE;
    if (s->type == CAP_NULL)
        return T_EBADCAP;
    if (s->type == CAP_UNTYPED || s->type == CAP_PAGE_TABLE)
        return T_EINVAL; /* memory and page-table authority is never duplicated */
    if (badge && s->type != CAP_ENDPOINT)
        return T_EINVAL; /* only endpoints carry badges */
    if (!(s->rights & CAP_R_GRANT))
        return T_EPERM;
    if (d->type != CAP_NULL)
        return T_EBUSY;
    /* A badge identifies a client to a server; once set it must not be
     * rewritten, or a client could impersonate another. */
    if (badge && s->badge)
        return T_EPERM;


    memset(d, 0, sizeof(*d));
    d->type = s->type;
    d->object = s->object;
    d->rights = s->rights & mask; /* monotonic: rights only ever shrink */
    if (s->type == CAP_ENDPOINT)
        d->badge = badge ? badge : s->badge;
    /* Everything else (mapping state, map_entry) stays zero: a copy holds
     * authority, not the source's mappings. */
    cap_insert_child(s, d);
    return T_OK;
}

int cap_delete(struct cspace *cs, cap_slot_t slot)
{
    struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;
    if (c->type == CAP_UNTYPED && cap_has_children(c))
        return T_EBUSY;

    destroy_slot(c);
    return T_OK;
}

int cap_revoke(struct cspace *cs, cap_slot_t slot, uint32_t budget)
{
    struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    return revoke(c, budget);
}
