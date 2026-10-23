/*
 * SYSCALL entry and the two return paths to user mode (ADR 0008).
 *
 * Selectors (gdt.h): user CS = 0x23, user SS = 0x1b.
 * Per-CPU offsets (percpu.h): 8 = kernel stack top, 16 = user RSP scratch.
 * struct user_regs offsets (thread.h): see R_* below.
 */

#define UCS 0x23
#define USS 0x1b
#define PCPU_KSTACK 8
#define PCPU_URSP   16
#define VEC_SYSCALL 0x100

#define R_RAX 0
#define R_RBX 8
#define R_RCX 16
#define R_RDX 24
#define R_RSI 32
#define R_RDI 40
#define R_RBP 48
#define R_RSP 56
#define R_R8  64
#define R_R9  72
#define R_R10 80
#define R_R11 88
#define R_R12 96
#define R_R13 104
#define R_R14 112
#define R_R15 120
#define R_RIP 128

.section .text

/*
 * SYSCALL lands here with IF, TF, DF, AC and NT already cleared by SFMASK
 * (T19), RCX = user RIP, R11 = user RFLAGS, and RSP still the user's.
 * Build the same frame an exception would, then go to common code (T32).
 */
.global syscall_entry
syscall_entry:
    endbr64
    swapgs
    movq %rsp, %gs:PCPU_URSP
    movq %gs:PCPU_KSTACK, %rsp
    clac
    cld
    pushq $USS
    pushq %gs:PCPU_URSP
    pushq %r11
    pushq $UCS
    pushq %rcx
    pushq $0
    pushq $VEC_SYSCALL
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
    movq %rsp, %rdi
    call entry_from_user      /* never returns */
    ud2

/*
 * void return_iret(const struct user_regs *r, uint64_t rflags) (noreturn)
 * Full IRETQ frame from validated state (T11-T18). Starts from the top of
 * the per-CPU kernel stack: nothing below is needed any more.
 */
.global return_iret
return_iret:
    movq %gs:PCPU_KSTACK, %rsp
    pushq $USS
    pushq R_RSP(%rdi)
    pushq %rsi
    pushq $UCS
    pushq R_RIP(%rdi)
    movq R_RAX(%rdi), %rax
    movq R_RBX(%rdi), %rbx
    movq R_RCX(%rdi), %rcx
    movq R_RDX(%rdi), %rdx
    movq R_RSI(%rdi), %rsi
    movq R_RBP(%rdi), %rbp
    movq R_R8(%rdi), %r8
    movq R_R9(%rdi), %r9
    movq R_R10(%rdi), %r10
    movq R_R11(%rdi), %r11
    movq R_R12(%rdi), %r12
    movq R_R13(%rdi), %r13
    movq R_R14(%rdi), %r14
    movq R_R15(%rdi), %r15
    movq R_RDI(%rdi), %rdi
    swapgs
    iretq

/*
 * void return_sysret(const struct user_regs *r, uint64_t rflags) (noreturn)
 * Only called after C has proven SYSRET safe (T18): RIP < USER_TOP and
 * rflags sanitised. RCX and R11 are clobbered per the ABI (T20). After RSP
 * becomes the user's, IF=0, so only NMI/#MC can arrive; both use IST stacks
 * and decide GS from the MSR.
 */
.global return_sysret
return_sysret:
    movq R_RIP(%rdi), %rcx
    movq %rsi, %r11
    movq R_RAX(%rdi), %rax
    movq R_RBX(%rdi), %rbx
    movq R_RDX(%rdi), %rdx
    movq R_RSI(%rdi), %rsi
    movq R_RBP(%rdi), %rbp
    movq R_R8(%rdi), %r8
    movq R_R9(%rdi), %r9
    movq R_R10(%rdi), %r10
    movq R_R12(%rdi), %r12
    movq R_R13(%rdi), %r13
    movq R_R14(%rdi), %r14
    movq R_R15(%rdi), %r15
    movq R_RSP(%rdi), %rsp
    movq R_RDI(%rdi), %rdi
    swapgs
    sysretq

/*
 * Kernel continuations: let a kernel context (the boot-time scheduler loop)
 * hand the CPU to user threads and get it back when none is runnable.
 * int kcont_save(struct kcont *k): 0 when saving, 1 when resumed.
 * void kcont_resume(struct kcont *k) (noreturn)
 */
.global kcont_save
kcont_save:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

.global kcont_resume
kcont_resume:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl $1, %eax
    jmp *56(%rdi)

.section .note.GNU-stack,"",@progbits
