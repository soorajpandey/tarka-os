#ifndef TARKA_ERRNO_H
#define TARKA_ERRNO_H

/* Kernel status codes. Functions return 0 on success or a negative
 * T_E* value; these values are part of the future syscall ABI. */
enum {
    T_OK         = 0,
    T_EINVAL     = -1,  /* malformed argument */
    T_ENOMEM     = -2,  /* out of memory */
    T_EPERM      = -3,  /* capability lacks the required rights */
    T_EBADCAP    = -4,  /* slot empty or of the wrong type */
    T_ERANGE     = -5,  /* index or address out of range */
    T_EBUSY      = -6,  /* destination slot occupied */
    T_EDOUBLEFREE = -7, /* object already free */
    T_EAGAIN     = -8,  /* partial progress; call again to continue */
};

#endif
