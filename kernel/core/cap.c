#include <tarka/cap.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>

static cap_finaliser_t finalisers[CAP_TYPE_COUNT];
static cap_cleanup_t cleanups[CAP_TYPE_COUNT];
static cap_binding_hook_t binding_hook;

void cap_set_binding_hook(cap_binding_hook_t fn)
{
    binding_hook = fn;
}

void cap_slot_clear(struct cap *c)
{
    uint32_t flags = c->slot_flags, aux = c->slot_aux;
    struct obj_meta meta = c->slot_meta;

    memset(c, 0, sizeof(*c));
    c->slot_flags = flags;
    c->slot_aux = aux;
    c->slot_meta = meta;
}

#ifdef TARKA_SELFTEST
/* Steps taken by revocation (ADR 0010 gate SC31.4, until stage 6 turns
 * it into the general work counter of SC27). */
uint64_t cdt_revoke_steps;
#endif

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
    KASSERT((uintptr_t)storage % _Alignof(struct cap) == 0);   /* SC20a */
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

/* ---- The CDT: a balanced-bracket list (ADR 0010, SC20) ----
 *
 * Every record contributes an open and a close element to one doubly
 * linked list, and the list is well-nested. A record's parent is the pair
 * that most tightly encloses its own. Each function below names the step
 * of SC20's proof it implements and does nothing else to the list (gate
 * SC31.1). Records are 128-byte aligned, so an element's offset within its
 * record says which bracket it is.
 */

struct cap *cdt_elem_cap(const struct cdt_elem *e)
{
    return (struct cap *)((uintptr_t)e & ~(uintptr_t)(sizeof(struct cap) - 1));
}

bool cdt_elem_is_open(const struct cdt_elem *e)
{
    return ((uintptr_t)e & (sizeof(struct cap) - 1)) == offsetof(struct cap, open);
}

static void elem_unlink(struct cdt_elem *e)
{
    if (e->prev)
        e->prev->next = e->next;
    if (e->next)
        e->next->prev = e->prev;
    e->prev = e->next = NULL;
}

/* Proof step 1 (base): a record with no parent is an adjacent pair "()". */
static void cdt_make_root(struct cap *c)
{
    c->open.prev = NULL;
    c->open.next = &c->close;
    c->close.prev = &c->open;
    c->close.next = NULL;
}

/* Proof steps 1-3: `child` becomes `parent`'s child as an adjacent pair
 * inserted immediately after open(parent). Nothing else moves. */
void cap_insert_child(struct cap *parent, struct cap *child)
{
    struct cdt_elem *after = parent->open.next;   /* at least close(parent) */

    KASSERT(after != NULL);
    child->open.prev = &parent->open;
    child->open.next = &child->close;
    child->close.prev = &child->open;
    child->close.next = after;
    after->prev = &child->close;
    parent->open.next = &child->open;
}

/* Proof steps 4-5: unlink both brackets. No other element moves, so every
 * remaining record keeps its enclosures, and c's children are now most
 * tightly enclosed by c's parent. O(1): no descendant is touched. */
static void cdt_remove(struct cap *c)
{
    elem_unlink(&c->open);
    elem_unlink(&c->close);
}

bool cap_has_children(const struct cap *c)
{
    return c->open.next != &c->close;
}

/* The record whose open element directly follows open(c): by
 * well-nestedness, a child of c (NULL if c has none). */
struct cap *cap_first_child(const struct cap *c)
{
    if (!cap_has_children(c))
        return NULL;
    KASSERT(cdt_elem_is_open(c->open.next));
    return cdt_elem_cap(c->open.next);
}

static void destroy_slot(struct cap *c)
{
    enum cap_type type = c->type;

    /* A thread is about to lose a binding: stop it first (ADR 0008, T2). */
    if ((c->slot_flags & SLOT_BINDING) && binding_hook)
        binding_hook(c);

    /* Undo per-capability state (e.g. a mapping) while the capability is
     * still intact and counted. */
    if (cleanups[type])
        cleanups[type](c);

    /* SC19a: this is the last capability to the object exactly when its
     * live-record count goes from 1 to 0. Nothing about c's place in the
     * CDT enters into it (gate SC31.2). The count changes in the same
     * step as the unlinking (C4). */
    struct obj_meta *m = obj_meta(c);
    KASSERT(m != NULL || !finalisers[type]);
    bool final = meta_del(c);
    uint64_t object = c->object;

    cdt_remove(c);
    /* Clear the slot before the finaliser runs so nothing can observe or
     * reuse a half-deleted capability. */
    cap_slot_clear(c);

    if (final && finalisers[type])
        finalisers[type](type, object);
    /* The teardown is complete: only now is the metadata cleared. (A
     * CNode's is cleared by its drain, which may finish later.) */
    if (final && m && type != CAP_CNODE)
        meta_retire(m);
}

/* SC20 corollary: the next record a revocation of c deletes. It is c's
 * first child, except that an Untyped which still has descendants is
 * never deleted (O8): revocation descends into it instead. That descent
 * is bounded: each nested Untyped is strictly smaller than its parent. */
static struct cap *revoke_victim(struct cap *c)
{
    struct cap *v = cap_first_child(c);

    while (v->type == CAP_UNTYPED && cap_has_children(v)) {
#ifdef TARKA_SELFTEST
        cdt_revoke_steps++;
#endif
        v = cap_first_child(v);
    }
    return v;
}

/* Delete c's descendants, one first child per step, with no search for
 * the end of the subtree (ADR 0010 SC22): each step is O(1) apart from
 * the bounded Untyped descent. Every intermediate state is a valid tree.
 * A finaliser may delete further records, even c itself (O11); the loop
 * re-reads c every step. Until stage 4, `budget` 0 still means "until
 * done". */
static int revoke(struct cap *c, uint32_t budget)
{
    for (uint32_t done = 0; c->type != CAP_NULL && cap_has_children(c); done++) {
        if (budget && done == budget)
            return T_EAGAIN;
#ifdef TARKA_SELFTEST
        cdt_revoke_steps++;
#endif
        destroy_slot(revoke_victim(c));
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

/* Only roots that no retype could make: boot Untypeds, the debug console
 * (no object), and in test builds test tokens (no object; their roots may
 * share an object value). Every other object comes from retype, which
 * records its carver (F1, gate SC31.5). */
static bool installable(enum cap_type type)
{
#ifdef TARKA_SELFTEST
    if (type == CAP_TEST_TOKEN)
        return true;
#endif
    return type == CAP_UNTYPED || type == CAP_DEBUG_CONSOLE;
}

int cap_install(struct cspace *cs, cap_slot_t slot, enum cap_type type,
                cap_rights_t rights, uint64_t object)
{
    struct cap *c = cap_slot(cs, slot);

    if (!c)
        return T_ERANGE;
    if (!installable(type) || (rights & ~CAP_R_ALL))
        return T_EINVAL;
    if (c->type != CAP_NULL)
        return T_EBUSY;

    cap_slot_clear(c);
    c->type = type;
    c->rights = rights;
    c->object = object;
    cdt_make_root(c);
    /* A root Untyped is a new object with no carver: the top of every
     * carving chain (SC22a). */
    if (type == CAP_UNTYPED) {
        KASSERT(serial_check(1) == T_OK);   /* boot: the counter is fresh */
        meta_new_object(c, NULL, serial_take());
    }
    meta_add(c);
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
    out->badge = cap_badged_type(c->type) ? c->badge : 0;
    return T_OK;
}

/* ADR 0009 P49: derivation is a check, which reports every error, and a
 * commit, which cannot fail. `d` may be NULL to check the source alone
 * (a transfer whose destination is not known yet). */
int cap_derive_check(const struct cap *s, const struct cap *d, uint64_t badge)
{
    if (s->type == CAP_NULL)
        return T_EBADCAP;
    if (s->type == CAP_UNTYPED || s->type == CAP_PAGE_TABLE)
        return T_EINVAL; /* memory and page-table authority is never duplicated */
    if (badge && !cap_badged_type(s->type))
        return T_EINVAL; /* only endpoints and notifications carry badges */
    if (!(s->rights & CAP_R_GRANT))
        return T_EPERM;
    if (d && d->type != CAP_NULL)
        return T_EBUSY;
    /* A badge identifies a client to a server; once set it must not be
     * rewritten, or a client could impersonate another. */
    if (badge && s->badge)
        return T_EPERM;
    /* SC22a (frozen: T_EBUSY) and SC18 (no room in the count: T_ERANGE).
     * Every derivation passes here: derive, transfer and binding. */
    return meta_check_new(s);
}

void cap_derive_commit(struct cap *s, struct cap *d, cap_rights_t mask, uint64_t badge)
{
    KASSERT(d != NULL && cap_derive_check(s, d, badge) == T_OK);
    cap_slot_clear(d);
    d->type = s->type;
    d->object = s->object;
    d->rights = s->rights & mask; /* monotonic: rights only ever shrink */
    /* Badge rule (ADR 0009, P1): a copy of a badged capability carries
     * exactly that badge (re-badging was refused above); only an unbadged
     * capability can mint a badged copy. */
    if (cap_badged_type(s->type))
        d->badge = badge ? badge : s->badge;
    /* Everything else (mapping state, map_entry) stays zero: a copy holds
     * authority, not the source's mappings. */
    cap_insert_child(s, d);
    meta_add(d);   /* C4: counted in the step that links it in */
}

static int derive(struct cap *s, struct cap *d, cap_rights_t mask, uint64_t badge)
{
    int rc = cap_derive_check(s, d, badge);

    if (rc)
        return rc;
    cap_derive_commit(s, d, mask, badge);
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
    return derive(s, d, mask, badge);
}

int cap_derive_into(struct cap *src, struct cap *dst, cap_rights_t mask)
{
    return derive(src, dst, mask, 0);
}

void cap_slot_delete(struct cap *c)
{
    if (c->type != CAP_NULL)
        destroy_slot(c);
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
