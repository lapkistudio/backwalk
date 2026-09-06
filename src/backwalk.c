#ifndef _WIN32
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#endif
#include "backwalk/backwalk.h"

#ifndef _WIN32
#include <dlfcn.h>  // for dladdr, Dl_info
#endif
#include <stdbool.h>  // for bool, false, true
#include <stddef.h>   // for NULL, size_t
#include <stdint.h>   // for uintptr_t
#include <stdlib.h>   // for free, malloc

#ifdef __linux__
#include <fcntl.h>     // for O_RDONLY, open
#include <link.h>      // for ElfW, RTLD_DL_LINKMAP, link_map
#include <string.h>    // for memcpy
#include <sys/mman.h>  // for MAP_FAILED, MAP_PRIVATE, PROT_READ, mmap, munmap
#include <sys/stat.h>  // for fstat, struct stat
#include <unistd.h>    // for close
#endif

#ifndef _WIN32
#include "context.h"  // for context_get_ip, context_init, context_step
#else
#include "win.h"  // for bw_win_capture, bw_win_resolve
#endif
#include "common.h"  // for BW_NOINLINE
#include "debug.h"   // for BW_PRINT_FRAME

#ifdef __linux__
static bool bw_elf_sname(const char* path, uintptr_t bias, uintptr_t ip, char* out, size_t out_sz) {
    if (path == NULL || path[0] == '\0' || out_sz == 0) {
        return false;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    const size_t file_sz = (size_t)st.st_size;
    void* map = mmap(NULL, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return false;
    }

    const unsigned char* base = (const unsigned char*)map;
    bool ok = false;
    do {
        if (file_sz < sizeof(ElfW(Ehdr)) || base[EI_MAG0] != ELFMAG0 || base[EI_MAG1] != ELFMAG1 ||
            base[EI_MAG2] != ELFMAG2 || base[EI_MAG3] != ELFMAG3) {
            break;
        }

        const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)base;
        if (eh->e_shentsize != sizeof(ElfW(Shdr)) || eh->e_shoff == 0) {
            break;
        }

        const size_t sh_bytes = (size_t)eh->e_shnum * sizeof(ElfW(Shdr));
        if (eh->e_shoff > file_sz || sh_bytes > file_sz - eh->e_shoff) {
            break;
        }

        const ElfW(Shdr)* sh = (const ElfW(Shdr)*)(base + eh->e_shoff);
        const ElfW(Sym)* best = NULL;
        const char* strtab = NULL;
        size_t strtab_sz = 0;

        for (ElfW(Half) i = 0; i < eh->e_shnum; ++i) {
            if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_entsize != sizeof(ElfW(Sym)) || sh[i].sh_link >= eh->e_shnum) {
                continue;
            }

            const ElfW(Shdr)* str_sh = &sh[sh[i].sh_link];
            if (sh[i].sh_offset > file_sz || sh[i].sh_size > file_sz - sh[i].sh_offset ||
                str_sh->sh_offset > file_sz || str_sh->sh_size > file_sz - str_sh->sh_offset) {
                continue;
            }

            const ElfW(Sym)* syms = (const ElfW(Sym)*)(base + sh[i].sh_offset);
            const size_t nsym = sh[i].sh_size / sizeof(ElfW(Sym));
            strtab = (const char*)(base + str_sh->sh_offset);
            strtab_sz = str_sh->sh_size;

            for (size_t s = 0; s < nsym; ++s) {
                const unsigned type = (unsigned)(syms[s].st_info & 0xf);
                if (type != STT_FUNC || syms[s].st_shndx == SHN_UNDEF || syms[s].st_size == 0 ||
                    syms[s].st_name == 0 || syms[s].st_name >= strtab_sz) {
                    continue;
                }

                const uintptr_t start = bias + (uintptr_t)syms[s].st_value;
                if (ip < start || ip >= start + (uintptr_t)syms[s].st_size) {
                    continue;
                }

                if (best == NULL || start >= bias + (uintptr_t)best->st_value) {
                    best = &syms[s];
                }
            }
        }

        if (best != NULL && strtab != NULL && best->st_name < strtab_sz) {
            const char* name = strtab + best->st_name;
            size_t len = 0;
            while (len < strtab_sz - best->st_name && name[len] != '\0') {
                ++len;
            }
            if (len > 0) {
                if (len >= out_sz) {
                    len = out_sz - 1;
                }
                memcpy(out, name, len);
                out[len] = '\0';
                ok = true;
            }
        }
    } while (0);

    munmap(map, file_sz);
    return ok;
}
#endif

static bool bw_frame_process(uintptr_t ip, bw_backtrace_cb cb, void* arg) {
    uintptr_t mod_addr = 0;
    const char* fname = "?";
    const char* sname = "?";
#ifdef _WIN32
    char fname_buf[BW_WIN_FNAME_MAX];
    char sname_buf[BW_WIN_SNAME_MAX];
#else
    Dl_info info = {0};
#ifdef __linux__
    char sname_buf[256];
#endif
#endif

#ifdef _WIN32
    bw_win_resolve(ip, &mod_addr, fname_buf, sizeof(fname_buf), sname_buf, sizeof(sname_buf));
    fname = fname_buf;
    sname = sname_buf;
#else
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const void* probe = (const void*)(ip - 1);
#ifdef __linux__
    void* extra = NULL;
    if (dladdr1(probe, &info, &extra, RTLD_DL_LINKMAP)) {
        mod_addr = ip - (uintptr_t)info.dli_fbase;
        fname = info.dli_fname ? info.dli_fname : "?";
        if (info.dli_sname != NULL) {
            sname = info.dli_sname;
        } else {
            const uintptr_t bias =
                extra != NULL ? (uintptr_t)((struct link_map*)extra)->l_addr : (uintptr_t)info.dli_fbase;
            if (bw_elf_sname(info.dli_fname, bias, ip - 1, sname_buf, sizeof(sname_buf))) {
                sname = sname_buf;
            }
        }
    }
#else
    if (dladdr(probe, &info)) {
        mod_addr = ip - (uintptr_t)info.dli_fbase;
    }
    fname = info.dli_fname ? info.dli_fname : "?";
    sname = info.dli_sname ? info.dli_sname : "?";
#endif
#endif

    BW_PRINT_FRAME(mod_addr, fname, sname);

    if (cb && !cb(mod_addr, fname, sname, arg)) {
        return false;
    }

    return true;
}

BW_NOINLINE bool bw_backtrace(bw_backtrace_cb cb, void* arg) {
#ifdef _WIN32
    uintptr_t ips[BW_WIN_MAX_FRAMES];
    const size_t n = bw_win_capture(ips, BW_WIN_MAX_FRAMES);
    for (size_t i = 0; i < n; ++i) {
        if (!bw_frame_process(ips[i], cb, arg)) {
            return false;
        }
    }
#else
    context_t ctx;
    context_init(&ctx);

    while (context_step(&ctx)) {
        uintptr_t ip = context_get_ip(&ctx);
        if (!bw_frame_process(ip, cb, arg)) {
            return false;
        };
    }
#endif

    return true;
}

struct bw_context {
    uintptr_t* ip;
    size_t ip_cnt;
    size_t ip_max_cnt;
};

bw_context_t* mkbw_context(size_t depth) {
    bw_context_t* bw_ctx = malloc(sizeof(bw_context_t));
    if (bw_ctx == NULL) {
        return NULL;
    }
    uintptr_t* ip = malloc(sizeof(uintptr_t) * depth);
    if (ip == NULL) {
        free(bw_ctx);
        return NULL;
    }

    bw_ctx->ip = ip;
    bw_ctx->ip_cnt = 0;
    bw_ctx->ip_max_cnt = depth;

    return bw_ctx;
}

void bw_context_fini(bw_context_t* bw_ctx) {
    if (bw_ctx == NULL) {
        return;
    }

    if (bw_ctx->ip != NULL) {
        free(bw_ctx->ip);
    }

    free(bw_ctx);
}

BW_NOINLINE bool bw_backtrace_collect(bw_context_t* bw_ctx) {
#ifdef _WIN32
    size_t n = 0;

    if (bw_ctx == NULL || bw_ctx->ip == NULL || bw_ctx->ip_max_cnt == 0) {
        return false;
    }

    n = bw_win_capture(bw_ctx->ip, bw_ctx->ip_max_cnt);
    bw_ctx->ip_cnt = n;
    return n < bw_ctx->ip_max_cnt;
#else
    context_t ctx;
    context_init(&ctx);
    size_t ip_cnt = 0;

    while (context_step(&ctx)) {
        if (ip_cnt >= bw_ctx->ip_max_cnt) {
            return false;
        }
        uintptr_t ip = context_get_ip(&ctx);
        bw_ctx->ip[ip_cnt++] = ip;
        bw_ctx->ip_cnt = ip_cnt;
    }

    return true;
#endif
}

bool bw_backtrace_process(bw_context_t* bw_ctx, bw_backtrace_cb cb, void* arg) {
    for (size_t i = 0; i < bw_ctx->ip_cnt; ++i) {
        if (!bw_frame_process(bw_ctx->ip[i], cb, arg)) {
            return false;
        };
    }

    return true;
}
