#if defined(__arm__)

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
    str  r11, [r0]
        
    mov  pc,  lr

#endif // defined(__arm__)
