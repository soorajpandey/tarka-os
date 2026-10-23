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

#ifdef TARKA_SELFTEST
/*
 * Test-only (ADR 0008, T32): the same copy, but sending itself an NMI
 * after stac, so the NMI arrives inside the open-SMAP window. The NMI
 * handler must run with AC clear, and IRETQ must restore AC, or this
 * rep movsb from user memory faults (and, not being the recoverable
 * instruction, panics).
 * long copy_user_nmi_probe(dst, src, len, volatile uint32_t *icr_lo)
 *   icr_lo: xAPIC ICR low register, or NULL to use the x2APIC MSR.
 */
.global copy_user_nmi_probe
copy_user_nmi_probe:
    endbr64
    movq %rdx, %r8
    movq %rdi, %r9
    movq %rcx, %r10
    stac
    testq %r10, %r10
    jz 1f
    movl $0x40400, (%r10)       /* xAPIC ICR low: NMI, self */
    jmp 2f
1:
    movl $0x830, %ecx           /* x2APIC ICR: NMI, self */
    movl $0x40400, %eax
    xorl %edx, %edx
    wrmsr
2:
    movq %r9, %rdi
    movq %r8, %rcx
    rep movsb
    clac
    xorl %eax, %eax
    ret
#endif

.section .note.GNU-stack,"",@progbits
