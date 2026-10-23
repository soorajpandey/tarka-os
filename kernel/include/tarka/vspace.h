#ifndef TARKA_VSPACE_H
#define TARKA_VSPACE_H

#include <tarka/cap.h>

/* Address spaces, page tables, mapping and unmapping. The specification is
 * docs/adr/0007-address-spaces.md; rule numbers (V1..V15) refer to it. */

#define USER_TOP       0x0000800000000000ull /* first non-user address */
#define VSPACE_OBJ_BITS 13                   /* table page + shadow page */

/* Record the kernel's master PML4 and register cleanup/finaliser hooks.
 * Must run before the first AddressSpace is created. */
void vspace_boot(void);

/* Called by retype on a freshly zeroed AddressSpace object. */
void vspace_as_init(paddr_t as);

__must_check int vspace_install(struct cspace *cs, cap_slot_t as, cap_slot_t pt,
                                uaddr_t va, unsigned depth);
__must_check int vspace_uninstall(struct cspace *cs, cap_slot_t pt);
__must_check int vspace_map(struct cspace *cs, cap_slot_t as, cap_slot_t frame,
                            uaddr_t va, cap_rights_t rights);
__must_check int vspace_unmap(struct cspace *cs, cap_slot_t frame);
__must_check int vspace_protect(struct cspace *cs, cap_slot_t frame, cap_rights_t rights);

/* Load an AddressSpace into CR3, or return to the kernel's master PML4. */
__must_check int vspace_activate(struct cspace *cs, cap_slot_t as);
void vspace_activate_kernel(void);

/* Load the AddressSpace named by a capability (kernel use: the return
 * path, where the capability is a thread's binding). */
void vspace_activate_cap(const struct cap *as);

/* V6 / ADR 0009 P5: would a new capability record to `frame` (a binding)
 * with rights `want` make the frame both writable (by a mapping or as an
 * IPC buffer) and executable? Every existing record is checked, `frame`'s
 * own mapping included. */
bool vspace_frame_alias_conflict(struct cap *frame, cap_rights_t want);

/* Structural audit of one AddressSpace (ADR 0007 tests): kernel half equal
 * to the master and supervisor-only; every user entry backed by a shadow
 * pointer to the capability that filled it, and vice versa; every leaf a
 * Frame with U=1 and never W without NX. */
bool vspace_audit(const struct cspace *cs, cap_slot_t as);

#endif
