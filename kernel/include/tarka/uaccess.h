#ifndef TARKA_UACCESS_H
#define TARKA_UACCESS_H

#include <tarka/types.h>

/* User-memory access (ADR 0008, T25-T27, T35). The only way the kernel
 * reads or writes memory through a user address. */

#define COPY_MAX 4096

/* Copy `len` bytes from user address `src`. Returns 0, T_EINVAL if
 * len > COPY_MAX, or T_EFAULT if [src, src+len) is not entirely user-half
 * or not accessible. On failure `dst` is zeroed completely, so no partial
 * or stale bytes survive (T35). */
__must_check int copy_from_user(void *dst, uaddr_t src, size_t len);

/* Copy `len` bytes to user address `dst`. Same errors. On failure the
 * user destination may be partially written. */
__must_check int copy_to_user(uaddr_t dst, const void *src, size_t len);

/* #PF handler hook: true if a kernel-mode fault at `rip` touching `addr`
 * is a recoverable user-copy fault (T35). Pure, for testing. */
bool copy_fault_recoverable(uint64_t rip, uint64_t addr);
uint64_t copy_fault_landing(void);

#endif
