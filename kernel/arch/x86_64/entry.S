/*
 * Kernel entry from Limine. Limine leaves us in 64-bit long mode with the
 * kernel mapped in the higher half; we switch to our own stack before
 * running any C code.
 */

.section .text
.global _start
.type _start, @function
_start:
    endbr64
    cli
    leaq boot_stack_top(%rip), %rsp
    xorl %ebp, %ebp
    call kmain
1:  cli
    hlt
    jmp 1b

/*
 * Kernel stacks. Each stack has a page directly below it that
 * stack_guards_init() unmaps, so running off the end of a stack faults
 * instead of silently corrupting neighbouring data. Sizes are multiples of
 * the page size so every guard stays page-aligned.
 */
.macro GUARDED_STACK name, size
.global \name, \name\()_guard, \name\()_top
.align 4096
\name\()_guard:
    .skip 4096
\name:
    .skip \size
\name\()_top:
.endm

.section .bss
GUARDED_STACK boot_stack, 32768
GUARDED_STACK kstack_cpu0, 16384   /* TSS.RSP0: entries from user mode (T8) */
GUARDED_STACK df_stack, 16384
GUARDED_STACK nmi_stack, 16384
GUARDED_STACK mc_stack, 16384

.section .note.GNU-stack,"",@progbits
