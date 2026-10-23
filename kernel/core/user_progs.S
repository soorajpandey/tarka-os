/*
 * User-mode test programs (test builds only). Position-independent code,
 * copied by the self-tests into a Frame mapped RX at UCODE_VA, with data at
 * UDATA_VA and a stack below USTACK_TOP.
 *
 * Report:  rax = SYS_TEST_REPORT, rdi/rsi/rdx/r10 = four values.
 * Exit:    rax = SYS_EXIT.
 */
#ifdef TARKA_SELFTEST

#define SYS_YIELD  0
#define SYS_EXIT   1
#define SYS_REPORT 0x7F00
#define UDATA_VA   0x401000

.macro REPORT
    movl $SYS_REPORT, %eax
    syscall
.endm
.macro EXIT
    movl $SYS_EXIT, %eax
    syscall
    ud2
.endm
.macro PROG name
.global prog_\name\()_start, prog_\name\()_end
prog_\name\()_start:
.endm
.macro END name
prog_\name\()_end:
.endm

.section .rodata.user_progs, "a"

/* Privilege and flags as seen from ring 3. */
PROG hello
    movq %cs, %rdi
    movq %ss, %rsi
    pushfq
    popq %rdx
    movl $0x7a72, %r10d
    REPORT
    EXIT
END hello

/* T7, T28: a new thread starts with every register zero and the FPU in its
 * initial state. */
PROG fresh
    orq %rbx, %rax
    orq %rcx, %rax
    orq %rdx, %rax
    orq %rsi, %rax
    orq %rdi, %rax
    orq %rbp, %rax
    orq %r8, %rax
    orq %r9, %rax
    orq %r10, %rax
    orq %r11, %rax
    orq %r12, %rax
    orq %r13, %rax
    orq %r14, %rax
    orq %r15, %rax
    movq %rax, %rdi                 /* a: OR of all GPRs */
    por %xmm1, %xmm0
    por %xmm2, %xmm0
    por %xmm3, %xmm0
    por %xmm4, %xmm0
    por %xmm5, %xmm0
    por %xmm6, %xmm0
    por %xmm7, %xmm0
    por %xmm8, %xmm0
    por %xmm9, %xmm0
    por %xmm10, %xmm0
    por %xmm11, %xmm0
    por %xmm12, %xmm0
    por %xmm13, %xmm0
    por %xmm14, %xmm0
    por %xmm15, %xmm0
    movq %xmm0, %rsi
    pextrq $1, %xmm0, %rax
    orq %rax, %rsi                  /* b: OR of all XMM bits */
    subq $16, %rsp
    fnstcw (%rsp)
    movzwl (%rsp), %edx             /* c: x87 control word */
    stmxcsr 8(%rsp)
    movl 8(%rsp), %r10d             /* d: MXCSR */
    addq $16, %rsp
    REPORT
    EXIT
END fresh

/* T6/T7: thread A's vector and x87 state survives a switch to B; B never
 * sees it. A runs first, loads patterns, yields, then reports them. */
PROG fpu_a
    movabsq $0x1122334455667788, %rax
    movq %rax, %xmm0
    pinsrq $1, %rax, %xmm0
    vinsertf128 $1, %xmm0, %ymm1, %ymm1  /* upper half of ymm1 = pattern */
    fldpi
    movl $SYS_YIELD, %eax
    syscall
    movq %xmm0, %rdi                     /* a: xmm0 low */
    vextractf128 $1, %ymm1, %xmm2
    movq %xmm2, %rsi                     /* b: ymm1 upper */
    subq $16, %rsp
    fstpl (%rsp)
    movq (%rsp), %rdx                    /* c: x87 st0 (pi) */
    addq $16, %rsp
    movl $1, %r10d                       /* d: marks thread A */
    REPORT
    EXIT
END fpu_a

PROG fpu_b
    movq %xmm0, %rdi                     /* a: must be 0 */
    vextractf128 $1, %ymm1, %xmm2
    movq %xmm2, %rsi                     /* b: must be 0 */
    fnstsw %ax
    movzwl %ax, %edx                     /* c: x87 status (TOP = 0) */
    movl $2, %r10d                       /* d: marks thread B */
    REPORT
    EXIT
END fpu_b

/* T20: across 2000 syscalls with pseudo-random register contents, every
 * register except RAX (result), RCX and R11 (clobbered) is preserved, and
 * RAX is the syscall's result (0). */
PROG preserve
    movabsq $0x9E3779B97F4A7C15, %r15
    xorl %ecx, %ecx
    movq %rcx, -8(%rsp)              /* mismatch count */
    movq $2000, -16(%rsp)            /* iterations left */
    subq $32, %rsp
1:
    movabsq $6364136223846793005, %rax
    imulq %rax, %r15
    movabsq $1442695040888963407, %rax
    addq %rax, %r15
    movq %r15, %rbx
    rorq $7, %rbx
    movq %r15, %rbp
    rorq $13, %rbp
    movq %r15, %rdi
    rorq $17, %rdi
    movq %r15, %rsi
    rorq $19, %rsi
    movq %r15, %rdx
    rorq $23, %rdx
    movq %r15, %r8
    rorq $29, %r8
    movq %r15, %r9
    rorq $31, %r9
    movq %r15, %r10
    rorq $37, %r10
    movq %r15, %r12
    rorq $41, %r12
    movq %r15, %r13
    rorq $43, %r13
    movq %r15, %r14
    rorq $47, %r14
    movq %r15, (%rsp)                /* expected seed */
    movl $SYS_YIELD, %eax
    syscall
    /* Recompute every expected value from the saved seed and compare. */
    movq (%rsp), %rcx
    cmpq %rcx, %r15
    jne 8f
    testq %rax, %rax
    jne 8f
    movq %rcx, %r11
    rorq $7, %r11
    cmpq %r11, %rbx
    jne 8f
    movq %rcx, %r11
    rorq $13, %r11
    cmpq %r11, %rbp
    jne 8f
    movq %rcx, %r11
    rorq $17, %r11
    cmpq %r11, %rdi
    jne 8f
    movq %rcx, %r11
    rorq $19, %r11
    cmpq %r11, %rsi
    jne 8f
    movq %rcx, %r11
    rorq $23, %r11
    cmpq %r11, %rdx
    jne 8f
    movq %rcx, %r11
    rorq $29, %r11
    cmpq %r11, %r8
    jne 8f
    movq %rcx, %r11
    rorq $31, %r11
    cmpq %r11, %r9
    jne 8f
    movq %rcx, %r11
    rorq $37, %r11
    cmpq %r11, %r10
    jne 8f
    movq %rcx, %r11
    rorq $41, %r11
    cmpq %r11, %r12
    jne 8f
    movq %rcx, %r11
    rorq $43, %r11
    cmpq %r11, %r13
    jne 8f
    movq %rcx, %r11
    rorq $47, %r11
    cmpq %r11, %r14
    jne 8f
    jmp 9f
8:
    incq 24(%rsp)                    /* mismatch */
9:
    decq 16(%rsp)
    jnz 1b
    movq 24(%rsp), %rdi              /* a: mismatches */
    movq $2000, %rsi                 /* b: iterations */
    xorl %edx, %edx
    xorl %r10d, %r10d
    addq $32, %rsp
    REPORT
    EXIT
END preserve

/* T9/T32: AC set in user mode never reaches the kernel. Report via a
 * syscall with AC set, then fault with AC still set. */
PROG ac
    pushfq
    orq $0x40000, (%rsp)
    popfq
    movl $0xac, %edi
    REPORT
    ud2
END ac

/* T15 and swapgs: FS and GS bases are the thread's own. Reads the words at
 * %fs:0 and %gs:0 (set up by the test to point into the data page). If GS
 * were the kernel's, this would read kernel per-CPU data or fault. */
PROG segbase
    movq %fs:0, %rdi
    movq %gs:0, %rsi
    REPORT
    EXIT
END segbase

/* T31: an unknown syscall number is an error return, not a fault. */
PROG badsys
    movl $0x7777, %eax
    syscall
    movq %rax, %rdi
    REPORT
    EXIT
END badsys

/* T29: one fault each. */
PROG f_ud
    ud2
END f_ud
PROG f_bp
    int3
END f_bp
PROG f_de
    xorl %ecx, %ecx
    divl %ecx
END f_de
PROG f_pf_read
    movq 0x10, %rax
END f_pf_read
PROG f_pf_write
    leaq 0(%rip), %rax
    movb $0, (%rax)
END f_pf_write
PROG f_pf_exec
    movq $UDATA_VA, %rax
    jmp *%rax
END f_pf_exec
PROG f_gp_hlt
    hlt
END f_gp_hlt
PROG f_gp_io
    inb $0x60, %al
END f_gp_io
PROG f_gp_int
    int $0x80
END f_gp_int
PROG f_gp_noncanon
    movabsq $0x8000000000000000, %rax
    movq (%rax), %rbx
END f_gp_noncanon
PROG f_ss
    movabsq $0x8000000000001000, %rsp
    pushq %rax
END f_ss
PROG f_xm
    subq $8, %rsp
    movl $0x1D00, (%rsp)          /* MXCSR default with invalid and divide-by-zero unmasked */
    ldmxcsr (%rsp)
    xorps %xmm0, %xmm0
    xorps %xmm1, %xmm1
    divss %xmm1, %xmm0             /* 0/0 */
    ud2                            /* reached only if #XM was not raised */
END f_xm
PROG f_ac
    pushfq
    orq $0x40000, (%rsp)
    popfq
    movl -15(%rsp), %eax           /* misaligned, inside the mapped stack */
    ud2                            /* reached only if #AC was not raised */
END f_ac

/* Spins long enough for a timer interrupt to arrive in ring 3, then
 * reports how far it got (a = iterations, proving it ran to the end). */
PROG spin
    movl $20000000, %ecx
    xorl %edi, %edi
1:
    incq %rdi
    decl %ecx
    jnz 1b
    REPORT
    EXIT
END spin

/* Syscall request runner: executes the list of requests at UDATA_VA,
 * 64 bytes each { nr, a0..a5, result }, until nr == -1, then exits. */
PROG runner
    movq $UDATA_VA, %rbx
1:
    movq 0(%rbx), %rax
    cmpq $-1, %rax
    je 2f
    movq 8(%rbx), %rdi
    movq 16(%rbx), %rsi
    movq 24(%rbx), %rdx
    movq 32(%rbx), %r10
    movq 40(%rbx), %r8
    movq 48(%rbx), %r9
    syscall
    movq %rax, 56(%rbx)
    addq $64, %rbx
    jmp 1b
2:
    EXIT
END runner

/* T18: placed so that this 2-byte syscall ends exactly at USER_TOP. */
PROG sysret_edge
    syscall
END sysret_edge

#endif
.section .note.GNU-stack,"",@progbits
