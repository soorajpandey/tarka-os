# Mutation results: stage6_layer1

Recorded from GitHub Actions run 40611687286 (workflow `mutation`), on
commit `9ad0820`; the kernel source is unchanged from layer 1 (`66c2a20`).
Regenerate with `scripts/mutate.py stage6_layer1`.

**Open finding, M19 (`test_arm_accepts_zero`).** As built, nothing in the
suite fails: `lapic_timer_arm`'s own `KASSERT(count > 0)` panics first.
Under SC31.6 that is not a kill. With the assertions disabled, `timer_hw`
fails on `sched_test_timer_arm(0) == T_EINVAL`. So the suite does catch
the behaviour, but the as-built run only shows the assertion. This is
reported, not reclassified; the workflow fails on it by design.
Commit `9ad0820`. Rule: ADR 0010 SC31.6. Blind mode disables KASSERT in: `kernel/core/sched.c`, `kernel/arch/x86_64/lapic.c`.

| ID | Mutant | File | Normal | Blind | Expected catcher |
|---|---|---|---|---|---|
| M01 | `setup_no_divide` | `lapic.c` | KILLED | KILLED | timer_boot (divider is not 0x3) |
| M02 | `setup_divide_by_1` | `lapic.c` | KILLED | KILLED | timer_boot (divider is not 0x3) |
| M03 | `setup_leaves_running` | `lapic.c` | KILLED | KILLED | timer_boot (boot count is not 0) |
| M04 | `arm_no_unmask` | `lapic.c` | KILLED | KILLED | timer_hw (no interrupt on expiry) |
| M05 | `arm_wrong_vector` | `lapic.c` | KILLED | KILLED | timer_hw (interrupt vector is not VEC_TIMER) |
| M06 | `arm_periodic` | `lapic.c` | KILLED | KILLED | timer_hw (count reloads after expiry) |
| M07 | `arm_count_scaled` | `lapic.c` | KILLED | KILLED | timer_hw (first count exceeds n) |
| M08 | `stop_no_zero` | `lapic.c` | KILLED | KILLED | timer_hw (stopped count is not 0) |
| M09 | `stop_no_mask` | `lapic.c` | EQUIVALENT | EQUIVALENT | none |
| M10 | `count_reads_initial` | `lapic.c` | KILLED | KILLED | timer_hw (count does not decrease) |
| M11 | `divide_wrong_bits` | `lapic.c` | KILLED | KILLED | timer_boot (divider is not 0x3) |
| M12 | `boot_no_setup` | `sched.c` | KILLED | KILLED | timer_boot (divider is not 0x3) |
| M13 | `boot_count_not_recorded` | `sched.c` | KILLED | KILLED | timer_boot (boot count is not 0) |
| M14 | `dispatch_gen_not_counted` | `sched.c` | KILLED | KILLED | timer_state (dispatch_gen does not match the trace) |
| M15 | `dispatch_gen_double` | `sched.c` | KILLED | KILLED | timer_state (dispatch_gen does not match the trace) |
| M16 | `dispatch_gen_counts_idle` | `sched.c` | KILLED | KILLED | timer_state (idle dispatches change dispatch_gen) |
| M17 | `test_arm_ignores_armed` | `sched.c` | KILLED | KILLED | timer_hw (arm not refused while a quantum runs) |
| M18 | `test_stop_ignores_armed` | `sched.c` | KILLED | KILLED | timer_hw (stop not refused while a quantum runs) |
| M19 | `test_arm_accepts_zero` | `sched.c` | ASSERT-ONLY | KILLED | timer_hw (arm(0) not refused); as built, lapic.c's assertion may fire first |
| M20 | `idle_wake_not_armed` | `sched.c` | KILLED | KILLED | sched_halt_wake (idle never wakes: hang) |
| M21 | `wake_armed_by_caller` | `sched.c` | KILLED | KILLED | sched_halt_wake (timer running before the HLT) |
| M22 | `audit_ignores_owner_gen` | `ipc.c` | KILLED | KILLED | sched_audit_detects (stale generation not detected) |

## Reasons

- **M01 `setup_no_divide`**
  - normal: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M02 `setup_divide_by_1`**
  - normal: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M03 `setup_leaves_running`**
  - normal: KILLED. test timer_boot: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M04 `arm_no_unmask`**
  - normal: KILLED. test entry_paths: FAIL entry_obs.user_irqs >= uirqs + 1 && entry_obs.last_user_irq == VEC_TIMER (kernel/core/selftest.c:2214)
  - blind: KILLED. test entry_paths: FAIL entry_obs.user_irqs >= uirqs + 1 && entry_obs.last_user_irq == VEC_TIMER (kernel/core/selftest.c:2214)
- **M05 `arm_wrong_vector`**
  - normal: KILLED. test entry_paths, sched_halt_wake, timer_hw: FAIL entry_obs.user_irqs >= uirqs + 1 && entry_obs.last_user_irq == VEC_TIMER (kernel/core/selftest.c:2214)
  - blind: KILLED. test entry_paths, sched_halt_wake, timer_hw: FAIL entry_obs.user_irqs >= uirqs + 1 && entry_obs.last_user_irq == VEC_TIMER (kernel/core/selftest.c:2214)
- **M06 `arm_periodic`**
  - normal: KILLED. test timer_hw: FAIL lapic_timer_count() == 0 && irq_window() == 0 (kernel/core/selftest.c:6809)
  - blind: KILLED. test timer_hw: FAIL lapic_timer_count() == 0 && irq_window() == 0 (kernel/core/selftest.c:6809)
- **M07 `arm_count_scaled`**
  - normal: KILLED. test timer_hw: FAIL c0 > 0 && c0 <= TMR_LONG (kernel/core/selftest.c:6814)
  - blind: KILLED. test timer_hw: FAIL c0 > 0 && c0 <= TMR_LONG (kernel/core/selftest.c:6814)
- **M08 `stop_no_zero`**
  - normal: KILLED. test timer_hw: FAIL lapic_timer_count() == 0 (kernel/core/selftest.c:6831)
  - blind: KILLED. test timer_hw: FAIL lapic_timer_count() == 0 (kernel/core/selftest.c:6831)
- **M09 `stop_no_mask`**
  - normal: EQUIVALENT. Writing an initial count of 0 stops the timer (Intel SDM vol. 3, 11.5.4), so no further interrupt can come from this arming whether or not the LVT is masked. An interrupt already raised is latched in IRR and unaffected by the mask either way. Every later arm() rewrites the LVT.
  - blind: EQUIVALENT. Writing an initial count of 0 stops the timer (Intel SDM vol. 3, 11.5.4), so no further interrupt can come from this arming whether or not the LVT is masked. An interrupt already raised is latched in IRR and unaffected by the mask either way. Every later arm() rewrites the LVT.
- **M10 `count_reads_initial`**
  - normal: KILLED. test timer_hw: FAIL down && prev < c0 && prev > 0 (kernel/core/selftest.c:6822)
  - blind: KILLED. test timer_hw: FAIL down && prev < c0 && prev > 0 (kernel/core/selftest.c:6822)
- **M11 `divide_wrong_bits`**
  - normal: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M12 `boot_no_setup`**
  - normal: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot, timer_hw: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M13 `boot_count_not_recorded`**
  - normal: KILLED. test timer_boot: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
  - blind: KILLED. test timer_boot: FAIL sched_boot_timer_count == 0 && lapic_timer_divide() == 0x3 (kernel/core/selftest.c:6786)
- **M14 `dispatch_gen_not_counted`**
  - normal: KILLED. test sched_audit_detects, timer_state: FAIL AUDIT() (kernel/core/selftest.c:6719)
  - blind: KILLED. test sched_audit_detects, timer_state: FAIL AUDIT() (kernel/core/selftest.c:6719)
- **M15 `dispatch_gen_double`**
  - normal: KILLED. test timer_state: FAIL b.dispatch_gen - a.dispatch_gen == sched_trace_n - n0 (kernel/core/selftest.c:6895)
  - blind: KILLED. test timer_state: FAIL b.dispatch_gen - a.dispatch_gen == sched_trace_n - n0 (kernel/core/selftest.c:6895)
- **M16 `dispatch_gen_counts_idle`**
  - normal: KILLED. test timer_state: FAIL b.dispatch_gen == a.dispatch_gen && !b.armed && AUDIT() (kernel/core/selftest.c:6886)
  - blind: KILLED. test timer_state: FAIL b.dispatch_gen == a.dispatch_gen && !b.armed && AUDIT() (kernel/core/selftest.c:6886)
- **M17 `test_arm_ignores_armed`**
  - normal: KILLED. test timer_hw: FAIL sched_test_timer_arm(TMR_LONG) == T_EBUSY && lapic_timer_count() == 0 (kernel/core/selftest.c:6866)
  - blind: KILLED. test timer_hw: FAIL sched_test_timer_arm(TMR_LONG) == T_EBUSY && lapic_timer_count() == 0 (kernel/core/selftest.c:6866)
- **M18 `test_stop_ignores_armed`**
  - normal: KILLED. test timer_hw: FAIL sched_test_timer_stop() == T_EBUSY (kernel/core/selftest.c:6867)
  - blind: KILLED. test timer_hw: FAIL sched_test_timer_stop() == T_EBUSY (kernel/core/selftest.c:6867)
- **M19 `test_arm_accepts_zero`**
  - normal: ASSERT-ONLY. in timer_hw: assertion failed: present && count > 0 (kernel/arch/x86_64/lapic.c:121)
  - blind: KILLED. test timer_hw: FAIL sched_test_timer_arm(0) == T_EINVAL && lapic_timer_count() == 0 (kernel/core/selftest.c:6869)
- **M20 `idle_wake_not_armed`**
  - normal: KILLED. hang in sched_halt_wake (timeout)
  - blind: KILLED. hang in sched_halt_wake (timeout)
- **M21 `wake_armed_by_caller`**
  - normal: KILLED. test sched_halt_wake: FAIL lapic_timer_count() == 0 (kernel/core/selftest.c:6554)
  - blind: KILLED. test sched_halt_wake: FAIL lapic_timer_count() == 0 (kernel/core/selftest.c:6554)
- **M22 `audit_ignores_owner_gen`**
  - normal: KILLED. test sched_audit_detects: FAIL !AUDIT() (kernel/core/selftest.c:6717)
  - blind: KILLED. test sched_audit_detects: FAIL !AUDIT() (kernel/core/selftest.c:6717)

normal: 20/22 killed, 1 equivalent, 1 other.

blind: 21/22 killed, 1 equivalent, 0 other.
