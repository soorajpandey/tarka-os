#ifndef TARKA_CAP_H
#define TARKA_CAP_H

#include <tarka/types.h>

/*
 * Capabilities are the only form of authority in Tarka. A task can act on
 * a kernel object only through a capability held in its capability space
 * (cspace). There are no ambient permissions: no root user, no global
 * object IDs that bypass the cspace. See docs/adr/0002-capabilities.md.
 */

enum cap_type {
    CAP_NULL = 0,   /* empty slot */
    CAP_UNTYPED,    /* raw physical memory that can be retyped */
    CAP_FRAME,      /* a mappable physical page */
    CAP_ENDPOINT,   /* IPC endpoint */
    CAP_IRQ,        /* right to receive one interrupt line */
    CAP_IOPORT,     /* range of x86 I/O ports */
    CAP_CNODE,      /* another capability table */
    CAP_THREAD,     /* thread control */
    CAP_TYPE_COUNT,
};

typedef uint32_t cap_rights_t;

#define CAP_R_READ   (1u << 0)
#define CAP_R_WRITE  (1u << 1)
#define CAP_R_EXEC   (1u << 2)
#define CAP_R_GRANT  (1u << 3)  /* may be copied/derived to another slot */
#define CAP_R_ALL    (CAP_R_READ | CAP_R_WRITE | CAP_R_EXEC | CAP_R_GRANT)

struct cap {
    uint32_t     type;     /* enum cap_type */
    cap_rights_t rights;
    uint64_t     object;   /* kernel object reference (type-specific) */
    uint64_t     badge;    /* caller-visible tag, set once at derivation */
};

/* No padding: capabilities will be copied to user space (e.g. cap_inspect)
 * and padding bytes could otherwise carry stale kernel data. */
_Static_assert(sizeof(struct cap) == 24, "struct cap must have no padding");

struct cspace {
    struct cap *slots;
    uint32_t    nslots;
};

typedef uint32_t cap_slot_t;

void cspace_init(struct cspace *cs, struct cap *storage, uint32_t nslots);

/* Install a new root capability. Only the kernel calls this while
 * constructing the initial task; it is never reachable from user space. */
__must_check int cap_install(struct cspace *cs, cap_slot_t slot, enum cap_type type,
                             cap_rights_t rights, uint64_t object);

/* Validate that `slot` holds a capability of `type` carrying every right
 * in `need`. This is the single choke point for every syscall that takes
 * a capability argument. */
__must_check int cap_lookup(const struct cspace *cs, cap_slot_t slot, enum cap_type type,
                            cap_rights_t need, const struct cap **out);

/* Derive a copy of `src` into the empty slot `dst` with rights narrowed to
 * `src.rights & mask`. Rights can never be widened. Requires CAP_R_GRANT on
 * the source. A non-zero badge may only be set on an unbadged source. */
__must_check int cap_derive(struct cspace *cs, cap_slot_t src, cap_slot_t dst,
                            cap_rights_t mask, uint64_t badge);

__must_check int cap_delete(struct cspace *cs, cap_slot_t slot);

#endif
