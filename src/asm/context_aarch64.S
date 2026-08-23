#if defined(__aarch64__)

#if defined(__APPLE__)
#define ASMNAME _context_init_arch
#else
#define ASMNAME context_init_arch
#endif

.text
.globl ASMNAME
#if defined(__ELF__)
.type ASMNAME, %function
#endif
ASMNAME:
    # Stack base for caller
    str  x29, [x0]

    ret

#endif // defined(__aarch64__)
