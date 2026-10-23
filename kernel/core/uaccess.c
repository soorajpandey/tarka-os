#include <tarka/uaccess.h>

#include <tarka/errno.h>
#include <tarka/selftest.h>
#include <tarka/string.h>
#include <tarka/vspace.h>

extern long copy_user_raw(void *dst, const void *src, size_t len);
extern const char copy_user_insn[], copy_user_fixup[];

/* T26 step 1: the whole range must lie in the user half. */
static int check_range(uaddr_t u, size_t len)
{
    uint64_t end;

    if (len > COPY_MAX)
        return T_EINVAL;
    if (add_overflow(ua_raw(u), (uint64_t)len, &end) || end > USER_TOP)
        return T_EFAULT;
    return T_OK;
}

int copy_from_user(void *dst, uaddr_t src, size_t len)
{
    int rc = check_range(src, len);

    if (rc == T_OK && len)
        rc = (int)copy_user_raw(dst, (const void *)(uintptr_t)ua_raw(src), len);
    if (rc != T_OK)
        memset(dst, 0, len <= COPY_MAX ? len : 0);
    return rc;
}

int copy_to_user(uaddr_t dst, const void *src, size_t len)
{
    int rc = check_range(dst, len);

    if (rc == T_OK && len)
        rc = (int)copy_user_raw((void *)(uintptr_t)ua_raw(dst), src, len);
    return rc;
}

bool copy_fault_recoverable(uint64_t rip, uint64_t addr)
{
    /* Keyed on the faulting instruction, not on a flag that could be left
     * set; and never for a kernel-half address, so a range-check bug can
     * never be turned into a silent error return. */
    return rip == (uint64_t)copy_user_insn && addr < USER_TOP;
}

uint64_t copy_fault_landing(void)
{
    return (uint64_t)copy_user_fixup;
}
