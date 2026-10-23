/*
 * The only code in the kernel that opens SMAP (ADR 0008, T26, T35).
 *
 * long copy_user_raw(void *dst, const void *src, size_t len)
 *   Leaf routine: calls nothing, so copies cannot nest and nothing else
 *   ever runs with AC set. Returns 0, or T_EFAULT via the landing pad if
 *   the single copying instruction faulted on a user address.
 *
 * The #PF handler recovers a fault only if RIP is exactly copy_user_insn
 * (the rep movsb) and the fault address is in the user half; it then
 * resumes at copy_user_fixup, which restores SMAP before returning.
 */

#define T_EFAULT -10

.section .text
.global copy_user_raw, copy_user_insn, copy_user_fixup
copy_user_raw:
    endbr64
    movq %rdx, %rcx
    stac
copy_user_insn:
    rep movsb
    clac
    xorl %eax, %eax
    ret
copy_user_fixup:
    clac
    movl $T_EFAULT, %eax
    ret

.section .note.GNU-stack,"",@progbits
