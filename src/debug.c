#include "debug.h"

#include <stdint.h>   // for uintptr_t, uintmax_t
#include <stdio.h>    // for NULL, fprintf, stderr

#include "common.h"  // for BW_UNUSED

#if !defined(_WIN32)
__attribute__((weak)) char* bw_dbg_demangle(const char* sname);
__attribute__((weak)) void bw_dbg_demangle_free(const char* sname);
#endif

void print_frame(uintptr_t addr, const char* fname, const char* sname) {
#if defined(_WIN32)
    BW_UNUSED(fprintf(stderr, "[%#08jx] %s:%s\n", (uintmax_t)addr, fname, sname));
#else
    char* demangled_sname = NULL;
    if (bw_dbg_demangle != NULL) {
        demangled_sname = bw_dbg_demangle(sname);
        if (demangled_sname != NULL) {
            sname = demangled_sname;
        }
    }

    BW_UNUSED(fprintf(stderr, "[%#08jx] %s:%s\n", (uintmax_t)addr, fname, sname));

    if (bw_dbg_demangle_free != NULL && demangled_sname != NULL) {
        bw_dbg_demangle_free(demangled_sname);
    }
#endif
}
