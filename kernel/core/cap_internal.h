#ifndef TARKA_CAP_INTERNAL_H
#define TARKA_CAP_INTERNAL_H

/* CDT primitives shared between cap.c and untyped.c. Not for use outside
 * the capability core. */

#include <tarka/cap.h>

/* Zero a slot's capability, keeping the slot's own properties
 * (slot_flags, slot_aux, slot_meta). Every writer of a slot uses
 * this. */
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

/* Derivation in two halves (ADR 0009, P49). cap_derive_check() reports
 * every error derivation can give (`d` NULL: source checks only);
 * cap_derive_commit() requires a passing check and cannot fail. */
__must_check int cap_derive_check(const struct cap *s, const struct cap *d, uint64_t badge);
void cap_derive_commit(struct cap *s, struct cap *d, cap_rights_t mask, uint64_t badge);

static inline bool cap_badged_type(uint32_t type)
{
#ifdef TARKA_SELFTEST
    if (type == CAP_TEST_TOKEN)
        return true;
#endif
    return type == CAP_ENDPOINT || type == CAP_NOTIFICATION;
}

/* ---- Per-object metadata (ADR 0010, SC17-SC19a, SC22a; objmeta.c) ---- */

/* Headroom kept below the counter's limit: one operation can create up to
 * three records of one object (a message's capabilities), and every
 * check runs before any of them is committed. */
#define OBJ_LIVE_MAX (UINT32_MAX - 3)

/* Allocate the frame metadata table (SC18). Runs before any Untyped
 * exists; boot fails if the table does not fit. */
void objmeta_boot(void);

/* The metadata of c's object, at its type's fixed location; NULL for a
 * type without one (debug console) and for an Untyped, whose metadata is
 * the struct ut_meta in its record. */
struct obj_meta *obj_meta(const struct cap *c);

/* SC22a F2: c's object, or an Untyped on its carving chain, is frozen. */
bool obj_frozen(const struct cap *c);

/* Every refusal a new record of s's object can meet: T_EBUSY if frozen,
 * T_ERANGE if the live count has no room. Called by cap_derive_check. */
__must_check int meta_check_new(const struct cap *s);

/* A freshly retyped (or boot) object: record its carving Untyped and its
 * serial (from serial_take(), after serial_check()). */
void meta_new_object(struct cap *c, struct cap *carver, uint64_t serial);

/* Serials (SC21, D3): T_ENOMEM unless n more remain; then take them one
 * at a time. Never reused, never wrapped. */
__must_check int serial_check(uint64_t n);
uint64_t serial_take(void);

/* c's object's serial (0 for the debug console), and the one identity
 * predicate (D0): same type, address and serial. */
uint64_t obj_serial(const struct cap *c);
bool obj_identity_is(const struct cap *c, uint32_t type, uint64_t object, uint64_t serial);

/* Commit steps (C4): c has just been linked in / is about to be unlinked.
 * meta_del() returns true when c was the object's last record; the
 * metadata stays until meta_retire(), when the object's teardown is
 * complete. */
void meta_add(struct cap *c);
bool meta_del(struct cap *c);
void meta_retire(struct obj_meta *m);

/* The frame metadata table entry of the page at `base` (SC18). */
struct obj_meta *frame_meta(paddr_t base);

/* Frame alias counts (SC18): a mapping with `rights` appears (+1) or goes
 * (-1); and the V6/P5 test for a new alias that wants `want`. */
void meta_alias(struct cap *frame, cap_rights_t rights, int delta);
bool meta_alias_conflict(const struct cap *frame, cap_rights_t want);
bool cap_is_ipcbuf_binding(const struct cap *c);

#ifdef TARKA_SELFTEST
void objmeta_test_freeze(const struct cap *c, bool frozen);
bool objmeta_token_entry(unsigned i, uint64_t *object, const struct obj_meta **m);
bool objmeta_frame_entry(uint64_t i, const struct obj_meta **m);
void objmeta_test_set_serial(uint64_t next);
uint64_t objmeta_test_next_serial(void);
#endif

/* The CDT as a balanced-bracket list (ADR 0010, SC20). */
struct cap *cdt_elem_cap(const struct cdt_elem *e);
bool cdt_elem_is_open(const struct cdt_elem *e);
struct cap *cap_first_child(const struct cap *c);

/* Link `child` into the CDT as `parent`'s first child: an adjacent pair
 * immediately after open(parent). */
void cap_insert_child(struct cap *parent, struct cap *child);

#ifdef TARKA_SELFTEST
extern uint64_t cdt_revoke_steps;
#endif

#endif
