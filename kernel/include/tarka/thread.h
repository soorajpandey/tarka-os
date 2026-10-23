#ifndef TARKA_THREAD_H
#define TARKA_THREAD_H

#include <tarka/cap.h>

/* Threads. Specification: docs/adr/0008-threads-user-entry-syscalls.md;
 * rule numbers (T1..T36) refer to it. */

#define THREAD_OBJ_BITS 13  /* page 0: TCB; page 1: XSAVE area */

/* The user-visible register set (T6). Exactly what thread_read_regs and
 * thread_write_regs exchange: no CS/SS (T14), no kernel state, no
 * padding. */
struct user_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;   /* user-settable bits only (T13) */
    uint64_t fs_base;
    uint64_t gs_base;
};
_Static_assert(sizeof(struct user_regs) == 20 * 8, "struct user_regs must have no padding");

/* RFLAGS bits a user may hold (T13): CF PF AF ZF SF DF OF AC ID. */
#define RFLAGS_USER_MASK ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | \
                          (1u << 10) | (1u << 11) | (1u << 18) | (1u << 21))
#define RFLAGS_IF        (1u << 9)
#define RFLAGS_FIXED     (1u << 1)   /* reserved, always 1 */

enum thread_state {
    THREAD_INACTIVE = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_FAULTED,
    THREAD_BLOCKED_SEND,    /* ADR 0009: in an endpoint's send queue */
    THREAD_BLOCKED_RECV,    /* in an endpoint's receive queue */
    THREAD_BLOCKED_REPLY,   /* linked to exactly one server */
    THREAD_BLOCKED_NOTIF,   /* in a notification's wait queue */
    THREAD_STATE_COUNT,
};

/* A wait queue (ADR 0009, P9). FIFO, intrusive through the TCB's single
 * wait link. `kind` is the one blocked state its members must be in. */
struct waitq {
    struct thread *head, *tail;
    uint32_t kind;          /* enum thread_state */
    uint32_t reserved;
};
_Static_assert(sizeof(struct waitq) == 24, "struct waitq must have no padding");

/* A message captured into kernel memory (ADR 0009, P2, P6, P7). */
#define MSG_MAX_WORDS 32
#define MSG_MAX_CAPS  3
struct ipc_msg {
    uint64_t info;                    /* label | caps | length (P3) */
    uint64_t words[MSG_MAX_WORDS];    /* zero beyond the length (P7) */
    uint32_t caps[MSG_MAX_CAPS];      /* sender's slots; zero beyond ncaps (P50) */
    uint32_t reserved;                /* always zero */
};
_Static_assert(sizeof(struct ipc_msg) == (1 + MSG_MAX_WORDS) * 8 + 16, "struct ipc_msg must have no padding");

/* A receiver's captured offer of empty slots (ADR 0009, P24, P50). */
struct ipc_offer {
    uint32_t slots[MSG_MAX_CAPS];     /* zero beyond count */
    uint32_t count;
};
_Static_assert(sizeof(struct ipc_offer) == 16, "struct ipc_offer must have no padding");

/* The IPC buffer layout (P4), at the start of a thread's TS_IPCBUF frame. */
struct ipc_buffer {
    uint64_t words[MSG_MAX_WORDS];
    uint32_t caps[MSG_MAX_CAPS];
    uint32_t recv_slots[MSG_MAX_CAPS];
    uint32_t recv_count;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ipc_buffer) == 32 * 8 + 8 * 4, "struct ipc_buffer must have no padding");
_Static_assert(sizeof(struct ipc_buffer) <= 4096, "an IPC buffer must fit the smallest frame");

/* A server's reply authority (P16, P17). Not a capability. */
enum reply_state {
    REPLY_NONE = 0,
    REPLY_PENDING,          /* reply_to is the caller, BLOCKED_REPLY */
    REPLY_CALLER_GONE,      /* authority held, but the caller is gone */
};
#define REPLY_CALL_GRANT (1u << 0)  /* the call carried GRANT (P23a) */

/* Binding slots (T1, T2). */
enum {
    TS_ASPACE = 0,   /* required */
    TS_CSPACE,       /* required */
    TS_IPCBUF,
    TS_FAULT,
    TS_COUNT,
};

/* What a user fault leaves behind (T29). ABI-visible: no padding. */
struct fault_record {
    uint64_t vector;
    uint64_t error;
    uint64_t addr;     /* CR2 for #PF, else 0 */
    uint64_t rip;
};
_Static_assert(sizeof(struct fault_record) == 32, "struct fault_record must have no padding");

/* How the thread's state must be restored on the way back (T18). */
enum return_path {
    RETURN_IRET = 0,  /* default: full, validated IRETQ frame */
    RETURN_SYSRET,    /* only after an unmodified SYSCALL entry */
};

struct thread {
    struct user_regs    regs;
    uint32_t            state;       /* enum thread_state */
    uint32_t            ret_path;    /* enum return_path */
    struct fault_record fault;
    struct thread      *rq_next;     /* run queue (internal references, O10) */
    struct thread      *rq_prev;
    /* IPC (ADR 0009). Every pointer here is an internal reference,
     * cleared before the thread or object it names can be freed. */
    struct waitq       *wait_q;      /* the queue this thread is in, or NULL */
    struct thread      *wait_next;
    struct thread      *wait_prev;
    struct thread      *reply_to;    /* server side: the caller to answer */
    struct thread      *reply_from;  /* caller side: the server answering */
    uint32_t            reply_state; /* enum reply_state */
    uint32_t            reply_flags;
    struct ipc_msg      msg;         /* a blocked sender's captured message (P6) */
    struct ipc_offer    offer;       /* a blocked receiver's captured slots (P50) */
    struct thread      *reg_next;    /* test builds: registry of live threads */
    struct thread      *reg_prev;
    struct cap          bind[TS_COUNT] __aligned(64);
};
_Static_assert(sizeof(struct thread) <= 4096, "TCB must fit in page 0 of the thread object");

/* Page 1 of the thread object. */
static inline void *thread_xsave_area(struct thread *t)
{
    return (uint8_t *)t + 4096;
}

/* Boot: register the binding hook and the thread finaliser. */
void thread_boot(void);

/* Retype hook: initialise a freshly zeroed thread object. */
void thread_obj_init(paddr_t obj);

/* True if every user-controlled value in `r` may be loaded on return to
 * user mode (T11-T15). */
bool user_regs_valid(const struct user_regs *r);

__must_check int thread_bind(struct cspace *cs, cap_slot_t thr, unsigned which,
                             struct cspace *src_cs, cap_slot_t src);
__must_check int thread_unbind(struct cspace *cs, cap_slot_t thr, unsigned which);
__must_check int thread_read_regs(struct cspace *cs, cap_slot_t thr, struct user_regs *out);
__must_check int thread_write_regs(struct cspace *cs, cap_slot_t thr, const struct user_regs *r);
__must_check int thread_resume(struct cspace *cs, cap_slot_t thr);
__must_check int thread_suspend(struct cspace *cs, cap_slot_t thr);
__must_check int thread_read_fault(struct cspace *cs, cap_slot_t thr, struct fault_record *out);
__must_check int thread_get_state(struct cspace *cs, cap_slot_t thr, uint32_t *out);

/* Scheduler interface (step 4: a minimal FIFO run queue). */
struct thread *runq_pop(void);
bool runq_empty(void);
bool runq_contains(const struct thread *t);
void thread_enqueue(struct thread *t);

/* T4: may this thread's saved state be loaded and run? */
bool thread_runnable_safe(const struct thread *t);

/* The thread object behind a Thread capability (kernel use only). */
struct thread *thread_of(const struct cap *c);

#endif
