/*
 * The user/kernel boundary (ADR 0008): entry from user mode, the return
 * path, user faults, and a minimal FIFO scheduler.
 */

#include <tarka/user.h>

#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "percpu.h"

#include <tarka/endpoint.h>
#include <tarka/errno.h>
#include <tarka/cont.h>
#include <tarka/kernel.h>
#include <tarka/reaper.h>
#include <tarka/sched.h>
#include <tarka/selftest.h>
#include <tarka/syscall.h>
#include <tarka/vspace.h>

#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u
#define EFER_SCE   (1ull << 0)

#define RFLAGS_TF  (1u << 8)
#define RFLAGS_DF  (1u << 10)
#define RFLAGS_NT  (1u << 14)
#define RFLAGS_AC  (1u << 18)

struct kcont { uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip; };

extern void syscall_entry(void);
extern __noreturn void return_iret(const struct user_regs *r, uint64_t rflags);
extern __noreturn void return_sysret(const struct user_regs *r, uint64_t rflags);
extern __attribute__((returns_twice)) int kcont_save(struct kcont *k);
extern __noreturn void kcont_resume(struct kcont *k);

static struct kcont idle_cont;
static bool sched_active;

static void pic_disable(void)
{
    /* Remap the legacy PICs away from the exception vectors (0x20-0x2F),
     * then mask every line. Nothing in Tarka uses them. */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
}

void user_boot(void)
{
    pic_disable();

    /* T19 */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, ((uint64_t)(GDT_UDATA - 8) << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_AC | RFLAGS_NT);

    /* SYSRET derives user CS/SS from STAR: check once that it yields
     * exactly the user selectors (T18). */
    uint64_t base = rdmsr(MSR_STAR) >> 48;
    KASSERT(((base + 16) | 3) == (GDT_UCODE | 3) && ((base + 8) | 3) == (GDT_UDATA | 3));
}

/* ---- T32: the sanitised entry state ---- */

#ifdef TARKA_SELFTEST
struct entry_observation entry_obs;
#endif

static void entry_state_check(const struct trap_frame *tf)
{
    extern char kstack_cpu0[], kstack_cpu0_top[];
    uint64_t rflags, rsp;

    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    bool ok = !(rflags & RFLAGS_AC) && !(rflags & RFLAGS_DF) &&
              rdmsr(MSR_GS_BASE) == (uint64_t)this_cpu() && this_cpu()->self == this_cpu() &&
              rsp > (uint64_t)kstack_cpu0 && rsp < (uint64_t)kstack_cpu0_top &&
              (tf->cs & 3) == 3;
#ifdef TARKA_SELFTEST
    entry_obs.entries++;
    entry_obs.last_rflags = rflags;
    entry_obs.last_user_rflags = tf->rflags;
    entry_obs.last_vector = tf->vector;
    if (!ok)
        entry_obs.violations++;
#endif
    KASSERT(ok);
}

/* ---- return path ---- */

/* Return `t` to user mode. Returns only if its state is not safe to load
 * (T17), after faulting it. */
static void try_exit_to_user(struct thread *t)
{
    if (!thread_runnable_safe(t)) {
        t->fault = (struct fault_record){ .vector = VEC_BAD_STATE, .rip = t->regs.rip };
        KASSERT(sched_fault(t) == T_OK);
        return;
    }

    vspace_activate_cap(&t->bind[TS_ASPACE]);
    fpu_restore(thread_xsave_area(t));
    wrmsr(MSR_FS_BASE, t->regs.fs_base);
    wrmsr(MSR_KERNEL_GS_BASE, t->regs.gs_base); /* becomes GS_BASE at swapgs */

    /* T13: user bits from the TCB, the rest forced. */
    uint64_t rflags = (t->regs.rflags & RFLAGS_USER_MASK) | RFLAGS_IF | RFLAGS_FIXED;

    /* T18: SYSRET only after an unmodified SYSCALL entry and with a
     * provably canonical user RCX. */
    if (t->ret_path == RETURN_SYSRET && t->regs.rip < USER_TOP) {
#ifdef TARKA_SELFTEST
        entry_obs.sysret_returns++;
#endif
        return_sysret(&t->regs, rflags);
    }
#ifdef TARKA_SELFTEST
    entry_obs.iret_returns++;
#endif
    return_iret(&t->regs, rflags);
}

/* G4: the idle thread with nothing to do. The real path halts until an
 * interrupt: `sti; hlt` is atomic, so no wake-up is lost. In test builds
 * the driver regains control here, at the pre-HLT boundary, and only
 * when no wake source is armed: the escape never stands in for an armed
 * halt. */
static __attribute__((noinline)) void idle_wait(void)
{
    KASSERT(runq_empty() && reaper_idle() && sched_current() == sched_idle());
#ifdef TARKA_SELFTEST
    if (!sched_wake_armed)
        kcont_resume(&idle_cont);   /* to the self-test driver */
    sched_wake_armed = false;
    sched_idle_halts++;
    sched_idle_arm_test_wake();     /* H1: a timer wake, armed by idle itself */
#endif
    /* An interrupt taken here returns to idle_hlt_resume. */
    __asm__ volatile("sti\n\thlt\n"
                     ".globl idle_hlt_resume\n"
                     "idle_hlt_resume:\n\t"
                     "cli" ::: "memory");
#ifdef TARKA_SELFTEST
    sched_idle_wakes++;
#endif
}

/* The scheduling boundary (ADR 0010 SC5, SC6, SC24; revision 3f). Every
 * kernel entry ends here. With nothing RUNNING, one reaper chunk, then a
 * dispatch: the run queue's head, or the idle thread, whose loop keeps
 * reaping one chunk per pass and otherwise waits. */
static __noreturn void schedule(void)
{
    struct thread *t = sched_current();

    if (t->state != THREAD_RUNNING)
        t = NULL;
    for (;;) {
        if (!t) {
            unsigned done = reaper_run(REAP_CHUNK);
            t = sched_dispatch();
            /* SC2, at the boundary: `current` is what was dispatched,
             * RUNNING or the idle thread, never NULL. */
            KASSERT(t && sched_current() == t &&
                    (t->state == THREAD_RUNNING || t->state == THREAD_IDLE));
            if (t->state == THREAD_IDLE) {
#ifdef TARKA_SELFTEST
                if (done) {
                    sched_idle_reap_passes++;
                    if (done > sched_reap_max_pass)
                        sched_reap_max_pass = done;
                }
#else
                (void)done;
#endif
                if (reaper_idle())
                    idle_wait();
                t = NULL;
                continue;
            }
        }
        /* ADR 0010 SC16, E4: a thread with a continuation runs its next
         * chunk instead of entering user mode. */
        if (t->cont.kind != CONT_NONE) {
            cont_run(t);
            if (t->state != THREAD_RUNNING) {
                t = NULL;
                continue;
            }
        }
        try_exit_to_user(t);   /* returns only if t was faulted */
        t = NULL;
    }
}

void sched_run(void)
{
    KASSERT(!sched_active);
    if (kcont_save(&idle_cont) == 0) {
        sched_active = true;
        schedule();
    }
    sched_active = false;
}

/* ---- entry ---- */

static void user_fault(struct thread *t, const struct trap_frame *tf)
{
    uint64_t cr2 = 0;

    if (tf->vector == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    /* P67: NMI, #DF and #MC take the paranoid path and never get here. */
    KASSERT(tf->vector != 2 && tf->vector != 8 && tf->vector != 18);
    t->fault = (struct fault_record){
        .vector = tf->vector, .error = tf->error, .addr = cr2, .rip = tf->rip,
    };
    /* ADR 0009 P69: a call to its handler, or FAULTED if it has none.
     * T29: the thread, never the kernel. */
    if (endpoint_fault(t) != T_OK)
        KASSERT(sched_fault(t) == T_OK);
}

void entry_from_user(struct trap_frame *tf);
void entry_from_user(struct trap_frame *tf)
{
    entry_state_check(tf);

    struct thread *t = this_cpu()->current;
    KASSERT(t && t->state == THREAD_RUNNING);

    /* T6: capture the whole context before anything else. */
    struct user_regs *r = &t->regs;
    r->rax = tf->rax; r->rbx = tf->rbx; r->rcx = tf->rcx; r->rdx = tf->rdx;
    r->rsi = tf->rsi; r->rdi = tf->rdi; r->rbp = tf->rbp; r->rsp = tf->rsp;
    r->r8 = tf->r8;   r->r9 = tf->r9;   r->r10 = tf->r10; r->r11 = tf->r11;
    r->r12 = tf->r12; r->r13 = tf->r13; r->r14 = tf->r14; r->r15 = tf->r15;
    r->rip = tf->rip;
    r->rflags = tf->rflags & RFLAGS_USER_MASK;
    r->fs_base = rdmsr(MSR_FS_BASE);
    r->gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    fpu_save(thread_xsave_area(t));

    if (tf->vector == VEC_SYSCALL) {
        t->ret_path = RETURN_SYSRET;    /* may be revoked by thread_write_regs */
        syscall_handle(t);
    } else {
        t->ret_path = RETURN_IRET;
        if (tf->vector < 32) {
            user_fault(t, tf);
        } else if (tf->vector != VEC_SPURIOUS) {
            /* An interrupt while user code ran: acknowledge it; the
             * thread continues (preemption arrives with step 6). */
#ifdef TARKA_SELFTEST
            entry_obs.user_irqs++;
            entry_obs.last_user_irq = tf->vector;
#endif
            lapic_eoi();
        }
    }
    schedule();
}
