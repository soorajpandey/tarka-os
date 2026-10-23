#ifndef TARKA_CAP_INTERNAL_H
#define TARKA_CAP_INTERNAL_H

/* CDT primitives shared between cap.c and untyped.c. Not for use outside
 * the capability core. */

#include <tarka/cap.h>

/* Zero a slot's capability, keeping the slot's own properties
 * (slot_flags, pending_link). Every writer of a slot uses this. */
void cap_slot_clear(struct cap *c);

/* Called for a capability in a binding slot (slot_flags & SLOT_BINDING)
 * just before it is deleted, while it is still intact. */
typedef void (*cap_binding_hook_t)(struct cap *c);
void cap_set_binding_hook(cap_binding_hook_t fn);

/* Derive `src` into `dst` (any slot, e.g. a binding slot): the
 * capability-level checks of cap_derive_to without slot lookup. */
__must_check int cap_derive_into(struct cap *src, struct cap *dst, cap_rights_t mask);

/* Delete the capability in a slot (any slot), running every hook. */
void cap_slot_delete(struct cap *c);

/* Bounds-checked slot access; NULL if `slot` is out of range. */
struct cap *cap_slot(const struct cspace *cs, cap_slot_t slot);

bool cap_has_children(const struct cap *c);

static inline bool cap_badged_type(uint32_t type)
{
#ifdef TARKA_SELFTEST
    if (type == CAP_TEST_TOKEN)
        return true;
#endif
    return type == CAP_ENDPOINT || type == CAP_NOTIFICATION;
}

/* True if both capabilities refer to the same object. */
bool cap_same_object(const struct cap *a, const struct cap *b);

/* All capabilities to one object are contiguous in the CDT; return the
 * first of them. Iterate with ->next while cap_same_object(). */
struct cap *cap_object_first(struct cap *c);

/* Link `child` into the CDT directly after `parent`, as its first child. */
void cap_insert_child(struct cap *parent, struct cap *child);

#endif
