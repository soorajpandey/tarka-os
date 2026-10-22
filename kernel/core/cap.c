#include <tarka/cap.h>

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>

static cap_finaliser_t finalisers[CAP_TYPE_COUNT];

void cap_set_finaliser(enum cap_type type, cap_finaliser_t fn)
{
    KASSERT(type > CAP_NULL && type < CAP_TYPE_COUNT);
    finalisers[type] = fn;
}

void cspace_init(struct cspace *cs, struct cap *storage, uint32_t nslots)
{
    memset(storage, 0, sizeof(*storage) * nslots);
    cs->slots = storage;
    cs->nslots = nslots;
}

static struct cap *slot_ptr(const struct cspace *cs, cap_slot_t slot)
{
    /* Slot indices will arrive straight from user registers: bounds-check
     * before every access. */
    if (slot >= cs->nslots)
        return NULL;
    return &cs->slots[slot];
}

static bool same_object(const struct cap *a, const struct cap *b)
{
    return a && b && a->type == b->type && a->object == b->object;
}

/*
 * All capabilities to one object are contiguous in the CDT: the kernel
 * installs one root per object and every copy is derived from it. So a
 * capability is the last one to its object exactly when neither CDT
 * neighbour refers to the same object.
 */
static bool is_final(const struct cap *c)
{
    return !same_object(c->prev, c) && !same_object(c->next, c);
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

/* Insert `child` directly after `parent`, as its first child. */
static void cdt_insert_child(struct cap *parent, struct cap *child)
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
    bool final = is_final(c);
    enum cap_type type = c->type;
    uint64_t object = c->object;

    cdt_remove(c);
    /* Clear the slot before the finaliser runs so nothing can observe or
     * reuse a half-deleted capability. */
    memset(c, 0, sizeof(*c));

    if (final && finalisers[type])
        finalisers[type](type, object);
}

void cspace_destroy(struct cspace *cs)
{
    for (uint32_t i = 0; i < cs->nslots; i++) {
        if (cs->slots[i].type != CAP_NULL)
            destroy_slot(&cs->slots[i]);
    }
    cs->slots = NULL;
    cs->nslots = 0;
}

int cap_install(struct cspace *cs, cap_slot_t slot, enum cap_type type,
                cap_rights_t rights, uint64_t object)
{
    struct cap *c = slot_ptr(cs, slot);

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
    const struct cap *c = slot_ptr(cs, slot);

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
    const struct cap *c = slot_ptr(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    memset(out, 0, sizeof(*out));
    out->type = c->type;
    out->rights = c->rights;
    out->badge = c->badge;
    return T_OK;
}

int cap_derive_to(struct cspace *src_cs, cap_slot_t src,
                  struct cspace *dst_cs, cap_slot_t dst,
                  cap_rights_t mask, uint64_t badge)
{
    struct cap *s = slot_ptr(src_cs, src);
    struct cap *d = slot_ptr(dst_cs, dst);

    if (!s || !d)
        return T_ERANGE;
    if (s->type == CAP_NULL)
        return T_EBADCAP;
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
    d->badge = badge ? badge : s->badge;
    cdt_insert_child(s, d);
    return T_OK;
}

int cap_delete(struct cspace *cs, cap_slot_t slot)
{
    struct cap *c = slot_ptr(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    destroy_slot(c);
    return T_OK;
}

int cap_revoke(struct cspace *cs, cap_slot_t slot, uint32_t budget)
{
    struct cap *c = slot_ptr(cs, slot);
    struct cap *last = c;

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    for (struct cap *n = c->next; n && n->depth > c->depth; n = n->next)
        last = n;

    /* Delete from the end of the subtree backwards. In pre-order every
     * node's descendants follow it, so each node is a leaf by the time it
     * is reached: nothing needs re-parenting, and stopping part-way leaves
     * a valid tree that the next call continues from. */
    for (uint32_t done = 0; last != c; done++) {
        if (budget && done == budget)
            return T_EAGAIN;
        struct cap *prev = last->prev;
        destroy_slot(last);
        last = prev;
    }
    return T_OK;
}
