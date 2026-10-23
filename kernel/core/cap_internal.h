#ifndef TARKA_CAP_INTERNAL_H
#define TARKA_CAP_INTERNAL_H

/* CDT primitives shared between cap.c and untyped.c. Not for use outside
 * the capability core. */

#include <tarka/cap.h>

/* Bounds-checked slot access; NULL if `slot` is out of range. */
struct cap *cap_slot(const struct cspace *cs, cap_slot_t slot);

bool cap_has_children(const struct cap *c);

/* True if both capabilities refer to the same object. */
bool cap_same_object(const struct cap *a, const struct cap *b);

/* All capabilities to one object are contiguous in the CDT; return the
 * first of them. Iterate with ->next while cap_same_object(). */
struct cap *cap_object_first(struct cap *c);

/* Link `child` into the CDT directly after `parent`, as its first child. */
void cap_insert_child(struct cap *parent, struct cap *child);

#endif
