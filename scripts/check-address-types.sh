#!/usr/bin/env bash
# ADR 0008, T34: confusing address kinds must be a compile error.
# Each snippet below misuses an address type and MUST fail to compile;
# the control snippet uses them correctly and MUST compile.
set -euo pipefail

CC="${CC:-gcc}"
FLAGS="-std=gnu11 -ffreestanding -fsyntax-only -Werror -Wall -Ikernel/include"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

prelude='#include <tarka/types.h>
extern void *phys_to_virt(paddr_t p);
extern int use_kptr(const void *p);'

check() { # name, expect (pass|fail), body
    printf '%s\nvoid f(paddr_t p, uaddr_t u, uint64_t n);\nvoid f(paddr_t p, uaddr_t u, uint64_t n) { (void)p; (void)u; (void)n; %s }\n' \
        "$prelude" "$3" > "$tmp/$1.c"
    if $CC $FLAGS "$tmp/$1.c" 2>/dev/null; then got=pass; else got=fail; fi
    if [ "$got" != "$2" ]; then
        echo "address-types: '$1' was expected to $2 compilation but did $got" >&2
        exit 1
    fi
}

check control           pass 'void *k = phys_to_virt(p); (void)k; uint64_t r = ua_raw(u) + pa_raw(p); (void)r;'
check deref_user        fail 'uint64_t v = *u; (void)v;'
check user_as_kptr      fail 'use_kptr(u);'
check user_cast_kptr    fail 'void *k = (void *)u; (void)k;'
check phys_as_user      fail 'uaddr_t x = p; (void)x;'
check user_as_phys      fail 'phys_to_virt(u);'
check int_as_phys       fail 'phys_to_virt(n);'
check phys_arith        fail 'paddr_t q = p + 4096; (void)q;'
check user_arith        fail 'uaddr_t v = u + 8; (void)v;'
check phys_compare      fail 'int b = p < p; (void)b;'
check phys_to_int       fail 'uint64_t x = p; (void)x;'

echo "address-types: 10 misuses rejected, control compiles"
