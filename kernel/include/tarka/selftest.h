#ifndef TARKA_SELFTEST_H
#define TARKA_SELFTEST_H

/* In-kernel test harness, compiled only into `make test` builds. Results
 * are reported over the serial port and the QEMU isa-debug-exit device so
 * CI can tell pass from fail. */

#include <tarka/types.h>

#ifdef TARKA_SELFTEST
__noreturn void selftest_run(void);

void selftest_check(bool ok, const char *expr, const char *file, int line);

/* Values user test programs report through SYS_TEST_REPORT. */
struct test_report {
    uint64_t a, b, c, d;          /* from the user program */
    uint64_t kernel_rflags;       /* RFLAGS in the kernel's common entry code */
    uint64_t user_rflags;         /* RFLAGS of the user context at entry */
};
#define TEST_REPORT_MAX 64
extern struct test_report test_reports[TEST_REPORT_MAX];
extern unsigned test_report_count;

/* What the kernel observed at user entries (ADR 0008, T32). */
struct entry_observation {
    uint64_t entries, violations;
    uint64_t last_rflags;        /* kernel RFLAGS in common code */
    uint64_t last_user_rflags;   /* RFLAGS of the interrupted user context */
    uint64_t last_vector;
    uint64_t sysret_returns, iret_returns;
    uint64_t nmis, nmi_last_rflags, nmi_last_from_user;
    uint64_t copy_faults;        /* recovered user-copy faults (T35) */
};
extern struct entry_observation entry_obs;
#define CHECK(cond) selftest_check((cond), #cond, __FILE__, __LINE__)
#endif

#endif
