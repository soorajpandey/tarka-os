#ifndef TARKA_TYPES_H
#define TARKA_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Address kinds are distinct types (ADR 0008, T34). Kernel virtual
 * addresses are ordinary C pointers. Physical and user addresses are
 * single-field structs: they cannot be dereferenced, mixed with each other
 * or with integers, and convert only through the explicit helpers below
 * (and phys_to_virt / virt_to_phys in kernel.h, copy_*_user for user
 * addresses).
 */
typedef struct { uint64_t raw; } paddr_t;   /* physical address */
typedef struct { uint64_t raw; } uaddr_t;   /* user virtual address */

static inline paddr_t pa(uint64_t raw) { return (paddr_t){ raw }; }
static inline uint64_t pa_raw(paddr_t p) { return p.raw; }
static inline bool pa_is_null(paddr_t p) { return p.raw == 0; }
static inline paddr_t pa_add(paddr_t p, uint64_t off) { return (paddr_t){ p.raw + off }; }

static inline uaddr_t ua(uint64_t raw) { return (uaddr_t){ raw }; }
static inline uint64_t ua_raw(uaddr_t u) { return u.raw; }

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
