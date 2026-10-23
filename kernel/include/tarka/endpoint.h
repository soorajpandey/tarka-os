#ifndef TARKA_ENDPOINT_H
#define TARKA_ENDPOINT_H

#include <tarka/cap.h>
#include <tarka/thread.h>

/*
 * Endpoints (ADR 0009, P13, P15, P26, P27, P27a, P38-P48): a rendezvous
 * point, not a mailbox. Two FIFO queues of blocked threads; at most one is
 * non-empty at any time. No message is ever stored in the endpoint.
 */

struct endpoint {
    struct waitq    senders;    /* kind = THREAD_BLOCKED_SEND */
    struct waitq    receivers;  /* kind = THREAD_BLOCKED_RECV */
    struct obj_meta meta;       /* live and rights counts (ADR 0010, SC17, SC19a) */
    uint64_t        reap_next;  /* the reaper link while listed (SC24) */
};
_Static_assert(sizeof(struct endpoint) <= 128, "endpoint must fit its 128-byte object");

/* Info word (P3). */
#define MSG_LEN(info)     ((unsigned)((info) & 0x3F))
#define MSG_NCAPS(info)   ((unsigned)(((info) >> 6) & 0x3))
#define MSG_LABEL(info)   ((uint32_t)((info) >> 32))
#define MSG_RESERVED_IN   0x00000000FFFFFF00ull

/* Output-only info bits (P3): never accepted on input. */
#define INFO_REPLY_SKIPPED (1ull << 10)   /* P63 */
#define INFO_FAULT         (1ull << 11)   /* a kernel-generated fault message (P68) */

/* The fault message (P68, frozen ABI). */
#define FAULT_LABEL 0xFFFFFFFFu
#define FAULT_WORDS 6   /* vector, error, address, RIP, RSP, RFLAGS */

void endpoint_boot(void);
void endpoint_obj_init(paddr_t obj);
struct endpoint *endpoint_of(const struct cap *c);

/* Live capability records conferring `right` on the endpoint (P27a): the
 * maintained count (ADR 0010, SC17). */
unsigned endpoint_caps_with(const struct cap *c, cap_rights_t right);

int64_t endpoint_send(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
int64_t endpoint_call(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
int64_t endpoint_reply(struct thread *t, struct cspace *cs, uint64_t info);
int64_t endpoint_reply_recv(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
__must_check int endpoint_fault(struct thread *t);
int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot);

#endif
