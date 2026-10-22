#ifndef TARKA_X86_64_SERIAL_H
#define TARKA_X86_64_SERIAL_H

#include <tarka/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);

#endif
