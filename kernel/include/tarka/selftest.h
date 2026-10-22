#ifndef TARKA_SELFTEST_H
#define TARKA_SELFTEST_H

/* In-kernel test harness, compiled only into `make test` builds. Results
 * are reported over the serial port and the QEMU isa-debug-exit device so
 * CI can tell pass from fail. */

#include <tarka/types.h>

#ifdef TARKA_SELFTEST
__noreturn void selftest_run(void);

void selftest_check(bool ok, const char *expr, const char *file, int line);
#define CHECK(cond) selftest_check((cond), #cond, __FILE__, __LINE__)
#endif

#endif
