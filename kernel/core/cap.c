#include <tarka/cap.h>

#include <tarka/errno.h>
#include <tarka/string.h>

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

    *c = (struct cap){ .type = type, .rights = rights, .object = object };
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

int cap_derive(struct cspace *cs, cap_slot_t src, cap_slot_t dst,
               cap_rights_t mask, uint64_t badge)
{
    struct cap *s = slot_ptr(cs, src);
    struct cap *d = slot_ptr(cs, dst);

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

    *d = *s;
    d->rights = s->rights & mask; /* monotonic: rights only ever shrink */
    if (badge)
        d->badge = badge;

    /* TODO: record d as a child of s in the derivation tree so revoking s
     * also revokes every capability derived from it. */
    return T_OK;
}

int cap_delete(struct cspace *cs, cap_slot_t slot)
{
    struct cap *c = slot_ptr(cs, slot);

    if (!c)
        return T_ERANGE;
    if (c->type == CAP_NULL)
        return T_EBADCAP;

    memset(c, 0, sizeof(*c));
    return T_OK;
}
