/*
 * Minimal, bounds-checked printf. Supports %c %s %d %i %u %x %X %p %%
 * with the '0', '-' and '#' flags, a numeric width, and the l/ll/z length
 * modifiers. Output is always NUL-terminated when size > 0 and never
 * writes past `size` bytes.
 */

#include <tarka/kernel.h>
#include <tarka/string.h>

#include "../arch/x86_64/serial.h"

struct out {
    char  *buf;
    size_t size;
    size_t len; /* characters that would have been written */
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_padded(struct out *o, const char *s, size_t n, int width, bool left, char pad)
{
    int fill = width > (int)n ? width - (int)n : 0;

    if (!left)
        while (fill-- > 0)
            put(o, pad);
    for (size_t i = 0; i < n; i++)
        put(o, s[i]);
    if (left)
        while (fill-- > 0)
            put(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { .buf = buf, .size = size, .len = 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }

        bool left = false, alt = false;
        char pad = ' ';
        int width = 0, lng = 0;

        for (;; fmt++) {
            if (fmt[1] == '-')
                left = true;
            else if (fmt[1] == '0')
                pad = '0';
            else if (fmt[1] == '#')
                alt = true;
            else
                break;
        }
        while (fmt[1] >= '0' && fmt[1] <= '9') {
            if (width < 1000)
                width = width * 10 + (fmt[1] - '0');
            fmt++;
        }
        while (fmt[1] == 'l' || fmt[1] == 'z') {
            lng++;
            fmt++;
        }
        fmt++;

        char tmp[24];
        size_t n = 0;
        unsigned base = 10;
        bool upper = false, neg = false;
        uint64_t v;

        switch (*fmt) {
        case '\0':
            fmt--;
            continue;
        case '%':
            put(&o, '%');
            continue;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            put_padded(&o, tmp, 1, width, left, ' ');
            continue;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            put_padded(&o, s, strlen(s), width, left, ' ');
            continue;
        }
        case 'd':
        case 'i': {
            int64_t sv = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            neg = sv < 0;
            v = neg ? -(uint64_t)sv : (uint64_t)sv;
            break;
        }
        case 'p':
            v = (uint64_t)va_arg(ap, void *);
            base = 16;
            alt = true;
            break;
        case 'X':
            upper = true;
            /* fallthrough */
        case 'x':
            base = 16;
            /* fallthrough */
        case 'u':
            v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            continue;
        }

        const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        do {
            tmp[sizeof(tmp) - 1 - n++] = digits[v % base];
            v /= base;
        } while (v);

        char prefix[3];
        size_t plen = 0;
        if (neg)
            prefix[plen++] = '-';
        if (alt && base == 16) {
            prefix[plen++] = '0';
            prefix[plen++] = 'x';
        }

        if (pad == '0' && !left) {
            for (size_t i = 0; i < plen; i++)
                put(&o, prefix[i]);
            put_padded(&o, tmp + sizeof(tmp) - n, n, width - (int)plen, false, '0');
        } else {
            char full[sizeof(tmp) + sizeof(prefix)];
            memcpy(full, prefix, plen);
            memcpy(full + plen, tmp + sizeof(tmp) - n, n);
            put_padded(&o, full, plen + n, width, left, ' ');
        }
    }

    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n >= (int)sizeof(buf))
        n = sizeof(buf) - 1;
    serial_write(buf, (size_t)n);
}
