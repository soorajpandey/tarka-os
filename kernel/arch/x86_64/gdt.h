#ifndef TARKA_X86_64_GDT_H
#define TARKA_X86_64_GDT_H

#include <tarka/types.h>

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x18
#define GDT_UCODE 0x20
#define GDT_TSS   0x28

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

void gdt_init(void);

#endif
