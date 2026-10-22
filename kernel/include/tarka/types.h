#ifndef TARKA_TYPES_H
#define TARKA_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

#define PAGE_SIZE ((uint64_t)4096)

#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + (a) - 1, (a))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))

#define __noreturn     __attribute__((noreturn))
#define __packed       __attribute__((packed))
#define __aligned(n)   __attribute__((aligned(n)))
#define __must_check   __attribute__((warn_unused_result))
#define __no_canary    __attribute__((no_stack_protector))
#define __unused       __attribute__((unused))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Overflow-checked arithmetic. Every size computation derived from
 * untrusted or bootloader-provided input must go through these. */
#define add_overflow(a, b, res) __builtin_add_overflow((a), (b), (res))
#define mul_overflow(a, b, res) __builtin_mul_overflow((a), (b), (res))

#endif
