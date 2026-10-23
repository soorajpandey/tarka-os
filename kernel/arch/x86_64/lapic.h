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
void lapic_timer_oneshot(uint32_t count);  /* fires VEC_TIMER */
void lapic_timer_stop(void);

/* xAPIC only: the ICR low register (test probe); NULL in x2APIC mode. */
volatile uint32_t *lapic_icr_lo_mmio(void);

#endif
