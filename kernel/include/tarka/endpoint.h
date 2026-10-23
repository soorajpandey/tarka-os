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
    struct waitq senders;      /* kind = THREAD_BLOCKED_SEND */
    struct waitq receivers;    /* kind = THREAD_BLOCKED_RECV */
};
_Static_assert(sizeof(struct endpoint) <= 64, "endpoint must fit its 64-byte object");

/* Info word (P3). */
#define MSG_LEN(info)     ((unsigned)((info) & 0x3F))
#define MSG_NCAPS(info)   ((unsigned)(((info) >> 6) & 0x3))
#define MSG_LABEL(info)   ((uint32_t)((info) >> 32))
#define MSG_RESERVED_IN   0x00000000FFFFFF00ull

void endpoint_boot(void);
void endpoint_obj_init(paddr_t obj);
struct endpoint *endpoint_of(const struct cap *c);

/* Live capability records conferring `right` on the endpoint (P27a). */
unsigned endpoint_caps_with(const struct cap *c, cap_rights_t right);

int64_t endpoint_send(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
int64_t endpoint_call(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
int64_t endpoint_reply(struct thread *t, struct cspace *cs, uint64_t info);
int64_t endpoint_reply_recv(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info);
int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot);

#endif
