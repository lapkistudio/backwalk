#if defined(__x86_64__)

#if defined(__APPLE__)
#define ASMNAME _context_init_arch
#else
#define ASMNAME context_init_arch
#endif

.text
.globl ASMNAME
ASMNAME:
    # Stack base for caller
    movq %rbp, (%rdi)

    ret

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

#endif // defined(__x86_64__)
