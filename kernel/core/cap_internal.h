#ifndef TARKA_CAP_INTERNAL_H
#define TARKA_CAP_INTERNAL_H

/* CDT primitives shared between cap.c and untyped.c. Not for use outside
 * the capability core. */

#include <tarka/cap.h>

/* Bounds-checked slot access; NULL if `slot` is out of range. */
struct cap *cap_slot(const struct cspace *cs, cap_slot_t slot);

bool cap_has_children(const struct cap *c);

/* Link `child` into the CDT directly after `parent`, as its first child. */
void cap_insert_child(struct cap *parent, struct cap *child);

#endif
