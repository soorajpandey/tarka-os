"""ADR 0010 stage 6 layer 1 mutants: the LAPIC timer interface and the
scheduler's ownership of it (revision 3g H1, A2; commit 66c2a20).

Each mutant replaces one exact piece of source text, which must occur
exactly once. `expect` names the test expected to catch it; the runner
records what actually did. `equivalent` marks a mutant argued to change
no observable behaviour, with the argument.
"""

LAPIC = "kernel/arch/x86_64/lapic.c"
SCHED = "kernel/core/sched.c"
IPC = "kernel/core/ipc.c"

# Files whose KASSERTs are disabled in blind mode (SC31.6).
BLIND = [SCHED, LAPIC]

MUTANTS = [
    # ---- lapic.c: hardware mechanics ----
    dict(id="M01", name="setup_no_divide", file=LAPIC,
         old="    wr(R_TMR_DIV, 0x3);                       /* divide by 16 */\n",
         new="",
         expect="timer_boot (divider is not 0x3)"),
    dict(id="M02", name="setup_divide_by_1", file=LAPIC,
         old="    wr(R_TMR_DIV, 0x3);                       /* divide by 16 */\n",
         new="    wr(R_TMR_DIV, 0xB);\n",
         expect="timer_boot (divider is not 0x3)"),
    dict(id="M03", name="setup_leaves_running", file=LAPIC,
         old="    wr(R_TMR_DIV, 0x3);                       /* divide by 16 */\n    lapic_timer_stop();\n",
         new="    wr(R_TMR_DIV, 0x3);\n    wr(R_LVT_TMR, VEC_TIMER);\n    wr(R_TMR_INIT, 100000000);\n",
         expect="timer_boot (boot count is not 0)"),
    dict(id="M04", name="arm_no_unmask", file=LAPIC,
         old="    wr(R_LVT_TMR, VEC_TIMER);                 /* unmasked, one-shot */\n",
         new="",
         expect="timer_hw (no interrupt on expiry)"),
    dict(id="M05", name="arm_wrong_vector", file=LAPIC,
         old="    wr(R_LVT_TMR, VEC_TIMER);                 /* unmasked, one-shot */\n",
         new="    wr(R_LVT_TMR, VEC_TEST_IPI);\n",
         expect="timer_hw (interrupt vector is not VEC_TIMER)"),
    dict(id="M06", name="arm_periodic", file=LAPIC,
         old="    wr(R_LVT_TMR, VEC_TIMER);                 /* unmasked, one-shot */\n",
         new="    wr(R_LVT_TMR, VEC_TIMER | (1u << 17));\n",
         expect="timer_hw (count reloads after expiry)"),
    dict(id="M07", name="arm_count_scaled", file=LAPIC,
         old="    wr(R_TMR_INIT, count);                    /* starts the count */\n",
         new="    wr(R_TMR_INIT, count * 4);\n",
         expect="timer_hw (first count exceeds n)"),
    dict(id="M08", name="stop_no_zero", file=LAPIC,
         old="    wr(R_TMR_INIT, 0);                        /* stops it; the count reads 0 */\n",
         new="",
         expect="timer_hw (stopped count is not 0)"),
    dict(id="M09", name="stop_no_mask", file=LAPIC,
         old="    wr(R_TMR_INIT, 0);                        /* stops it; the count reads 0 */\n    wr(R_LVT_TMR, LVT_MASKED | VEC_TIMER);\n",
         new="    wr(R_TMR_INIT, 0);\n",
         expect="none",
         equivalent="Writing an initial count of 0 stops the timer (Intel SDM "
                    "vol. 3, 11.5.4), so no further interrupt can come from this "
                    "arming whether or not the LVT is masked. An interrupt "
                    "already raised is latched in IRR and unaffected by the mask "
                    "either way. Every later arm() rewrites the LVT."),
    dict(id="M10", name="count_reads_initial", file=LAPIC,
         old="    return rd(R_TMR_CUR);\n",
         new="    return rd(R_TMR_INIT);\n",
         expect="timer_hw (count does not decrease)"),
    dict(id="M11", name="divide_wrong_bits", file=LAPIC,
         old="    return rd(R_TMR_DIV) & 0xB;   /* bits 0, 1 and 3 */\n",
         new="    return rd(R_TMR_DIV) & 0x1;\n",
         expect="timer_boot (divider is not 0x3)"),

    # ---- sched.c: ownership ----
    dict(id="M12", name="boot_no_setup", file=SCHED,
         old="        lapic_timer_setup();\n",
         new="",
         expect="timer_boot (divider is not 0x3)"),
    dict(id="M13", name="boot_count_not_recorded", file=SCHED,
         old="        sched_boot_timer_count = lapic_timer_count();\n",
         new="",
         expect="timer_boot (boot count is not 0)"),
    dict(id="M14", name="dispatch_gen_not_counted", file=SCHED,
         old="    c->timer.dispatch_gen++;   /* A2: counts non-idle dispatches only */\n",
         new="",
         expect="timer_state (dispatch_gen does not match the trace)"),
    dict(id="M15", name="dispatch_gen_double", file=SCHED,
         old="    c->timer.dispatch_gen++;   /* A2: counts non-idle dispatches only */\n",
         new="    c->timer.dispatch_gen += 2;\n",
         expect="timer_state (dispatch_gen does not match the trace)"),
    dict(id="M16", name="dispatch_gen_counts_idle", file=SCHED,
         old="        this_cpu()->current = &c->idle;   /* SC5: only when the queue is empty */\n",
         new="        this_cpu()->current = &c->idle;\n        c->timer.dispatch_gen++;\n",
         expect="timer_state (idle dispatches change dispatch_gen)"),
    dict(id="M17", name="test_arm_ignores_armed", file=SCHED,
         old="    if (this_sched()->timer.armed)\n        return T_EBUSY;\n    if (count == 0)\n",
         new="    if (count == 0)\n",
         expect="timer_hw (arm not refused while a quantum runs)"),
    dict(id="M18", name="test_stop_ignores_armed", file=SCHED,
         old="    if (this_sched()->timer.armed)\n        return T_EBUSY;\n    lapic_timer_stop();\n",
         new="    lapic_timer_stop();\n",
         expect="timer_hw (stop not refused while a quantum runs)"),
    # The scheduler's check still runs but reports success. Deleting the
    # check instead reaches lapic_timer_arm(0), whose KASSERT fires before
    # any CHECK as built: assertion-only under SC31.6 (run 40611687286).
    dict(id="M19", name="test_arm_accepts_zero", file=SCHED,
         old="    if (count == 0)\n        return T_EINVAL;\n",
         new="    if (count == 0)\n        return T_OK;\n",
         expect="timer_hw (arm(0) returns T_OK, not T_EINVAL)"),
    dict(id="M20", name="idle_wake_not_armed", file=SCHED,
         old="        lapic_timer_arm(c->test_wake);\n        c->test_wake = 0;\n",
         new="        c->test_wake = 0;\n",
         expect="sched_halt_wake (idle never wakes: hang)"),
    dict(id="M21", name="wake_armed_by_caller", file=SCHED,
         old="    this_sched()->test_wake = ticks;\n",
         new="    lapic_timer_arm(ticks);\n    this_sched()->test_wake = 0;\n",
         expect="sched_halt_wake (timer running before the HLT)"),

    # ---- ipc.c: the audit of the timer state (T36-2) ----
    dict(id="M22", name="audit_ignores_owner_gen", file=IPC,
         old="cur->state != THREAD_RUNNING || tm.owner_gen != tm.dispatch_gen ||\n",
         new="cur->state != THREAD_RUNNING ||\n",
         expect="sched_audit_detects (stale generation not detected)"),
]
