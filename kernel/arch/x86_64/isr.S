/*
 * Interrupt and exception entry (ADR 0008, T9, T32).
 *
 * Each of the 256 vectors has a stub that normalises the stack to
 *   [vector][error code][CPU frame: RIP CS RFLAGS RSP SS]
 * and jumps to one of two common paths. Both establish the sanitised
 * entry state before any C code runs:
 *   - AC = 0 (clac) and DF = 0 (cld), first thing;
 *   - the kernel's GS base;
 *   - a valid kernel stack (the CPU switched to TSS.RSP0 or an IST stack);
 *   - every general register saved.
 *
 * isr_common   ordinary vectors. GS is swapped iff the interrupted context
 *              was ring 3 (CS & 3). From ring 3 control goes to
 *              entry_from_user(), which never returns here; from ring 0 to
 *              isr_dispatch(), followed by IRETQ.
 * paranoid_common  NMI, #DF and #MC: they can interrupt kernel code at any
 *              instruction, including between a swapgs and the IRETQ/SYSRET
 *              that follows it, so CS says nothing reliable about GS. The
 *              GS-base MSR decides instead (kernel addresses are negative).
 */

#define TF_CS 24  /* offset of CS above [vector][error][rip] */

.macro STUB n, kind
.global isr\n
isr\n:
    endbr64
.if \kind == 0
    pushq $0              /* no error code: keep the frame uniform */
.endif
    pushq $\n
.if (\n == 2) || (\n == 8) || (\n == 18)
    jmp paranoid_common
.else
    jmp isr_common
.endif
.endm

.macro PUSH_GPRS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_GPRS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

.section .text

/* Vectors pushing an error code: 8, 10-14, 17, 21, 29, 30. */
.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    STUB \n, 0
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
    STUB \n, 1
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    STUB \n, 0
.endr
.irp n, 64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95
    STUB \n, 0
.endr
.irp n, 96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    STUB \n, 0
.endr
.irp n, 128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
    STUB \n, 0
.endr
.irp n, 160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191
    STUB \n, 0
.endr
.irp n, 192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223
    STUB \n, 0
.endr
.irp n, 224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
    STUB \n, 0
.endr

isr_common:
    clac
    cld
    testb $3, TF_CS(%rsp)
    jz 1f
    swapgs
    PUSH_GPRS
    movq %rsp, %rdi
    call entry_from_user      /* saves state into the TCB; never returns */
    ud2
1:
    PUSH_GPRS
    movq %rsp, %rdi
    call isr_dispatch         /* kernel-mode entry */
    POP_GPRS
    addq $16, %rsp
    iretq

paranoid_common:
    clac
    cld
    PUSH_GPRS
    movl $0xC0000101, %ecx    /* MSR_GS_BASE */
    rdmsr
    xorl %ebx, %ebx           /* rbx: callee-saved, survives the call */
    testl %edx, %edx
    js 2f                     /* already the kernel's (negative address) */
    swapgs
    movl $1, %ebx
2:
    movq %rsp, %rdi
    call paranoid_dispatch
    testl %ebx, %ebx
    jz 3f
    swapgs
3:
    POP_GPRS
    addq $16, %rsp
    iretq

.section .rodata
.global isr_table
.align 8
isr_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad isr\n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
    .quad isr\n
.endr
.irp n, 64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95
    .quad isr\n
.endr
.irp n, 96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    .quad isr\n
.endr
.irp n, 128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
    .quad isr\n
.endr
.irp n, 160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191
    .quad isr\n
.endr
.irp n, 192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223
    .quad isr\n
.endr
.irp n, 224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
    .quad isr\n
.endr

.section .note.GNU-stack,"",@progbits
