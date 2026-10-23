#ifndef TARKA_X86_64_LAPIC_H
#define TARKA_X86_64_LAPIC_H

#include <tarka/types.h>

/* Local APIC in x2APIC mode (MSR interface). */

#define VEC_TIMER    0x40
#define VEC_TEST_IPI 0x41
#define VEC_SPURIOUS 0xFF

bool lapic_init(void);          /* false if x2APIC is unavailable */
bool lapic_present(void);
void lapic_eoi(void);
void lapic_self_ipi(uint8_t vector);
void lapic_self_nmi(void);
/*
 * The LAPIC timer: hardware mechanics only. It knows nothing of threads,
 * quanta or owners (ADR 0010 revision 3g H1, A2). Only core/sched.c may
 * program it (gate: scripts/check-sched.sh).
 *
 * - lapic_timer_setup(): divide by 16, stopped, the LVT masked. Once, at
 *   boot.
 * - lapic_timer_arm(n), n > 0: one-shot. The current count starts at n
 *   and counts down to 0, where it stays; reaching 0 raises VEC_TIMER
 *   once. Arming again restarts the count from the new n.
 * - lapic_timer_stop(): no further interrupt from this arming, and the
 *   count reads 0. An interrupt already raised (latched in the LAPIC) is
 *   not withdrawn.
 * - lapic_timer_count(): the current count.
 * - lapic_timer_divide(): the divide configuration (3: divide by 16).
 *
 * A count of 0 looks the same whether the timer ran out or was stopped:
 * callers must tell the two apart by their own state.
 */
void lapic_timer_setup(void);
void lapic_timer_arm(uint32_t count);
void lapic_timer_stop(void);
uint32_t lapic_timer_count(void);
uint32_t lapic_timer_divide(void);   /* the divide configuration register */

/* xAPIC only: the ICR low register (test probe); NULL in x2APIC mode. */
volatile uint32_t *lapic_icr_lo_mmio(void);

#endif
