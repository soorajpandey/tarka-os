#ifndef TARKA_REAPER_H
#define TARKA_REAPER_H

#include <tarka/cap.h>

/*
 * The reaper (ADR 0010, SC23, SC24, R0-R9, revision 3d D2, D4, D5).
 *
 * Objects whose teardown is not O(1) do not die in the deleter's entry:
 * an endpoint or notification with waiters, and every CNode. When the
 * last record goes, the object becomes dying in the same step: marked,
 * its carving Untyped's dying count raised (which pins its memory, R4),
 * and appended to the reaper's list. The reaper then tears it down at
 * scheduling boundaries, at most REAP_CHUNK units at a time, and only
 * its completing unit drops the dying count. Count 0 is not reuse (D0).
 *
 * A live endpoint or notification whose waiters can never be served
 * (the opposite authority is gone) is listed too, with its doomed
 * direction, and the reaper fails those waiters one per unit.
 *
 * One list for now (one CPU); stage 5 makes it per-CPU.
 */

#define REAP_CHUNK 4   /* units per scheduling boundary (R5) */

/* Called by destroy_slot() for an object's last record, after its count
 * reached 0 and before the record is unlinked (R0). True if the object
 * is now dying and the reaper owns its teardown; false if it has none
 * pending and its finaliser should run now. */
bool reaper_take(struct cap *c, struct obj_meta *m);

/* Authority loss (SC23): the endpoint or notification `c` names has
 * waiters in direction `doom` (OBJ_DOOM_SEND / OBJ_DOOM_RECV) that can
 * never be served. List it if it is not listed. */
void reaper_doom(const struct cap *c, struct obj_meta *m, uint32_t doom);

/* At a scheduling boundary: perform at most `units` units. Returns the
 * number performed (0 when there is nothing to do). */
unsigned reaper_run(unsigned units);

bool reaper_idle(void);

#ifdef TARKA_SELFTEST
/* Walk the list: `*cursor` 0 to start; returns false at the end. */
bool reaper_list_next(uint64_t *cursor, unsigned *kind, const struct obj_meta **m);
#define REAP_KIND_EP    1
#define REAP_KIND_NTFN  2
#define REAP_KIND_CNODE 3

/* A trace of reaper decisions, so tests can check D2's proof at run
 * time: every unit records the serial of the object it worked on and
 * what it did. */
extern uint64_t reaper_units_total;   /* units ever performed */
#define REAP_TRACE_MAX 512
enum reap_act { REAP_REAL = 1, REAP_REQUEUE, REAP_DONE };
struct reap_event { uint64_t serial; uint32_t act; uint32_t kind; };
extern struct reap_event reap_trace[REAP_TRACE_MAX];
extern unsigned reap_trace_n;

/* Run until the list is empty (tests). Fails the boot if that takes more
 * than `max_units`: a sign the reaper does not terminate. */
unsigned reaper_drain(unsigned long max_units);
#endif

#endif
