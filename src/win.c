#include "win.h"

#include "common.h"  // for BW_NOINLINE
#include "dwarf_line.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")
#endif

// Skip bw_win_capture and its caller (bw_backtrace / bw_backtrace_collect).
enum { BW_WIN_SKIP = 2 };

static INIT_ONCE bw_sym_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION bw_sym_cs;
static BOOL bw_sym_ok = FALSE;

static BW_NO_SANITIZE_ADDRESS BOOL CALLBACK bw_sym_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once;
    (void)param;
    (void)ctx;

    InitializeCriticalSection(&bw_sym_cs);
    (void)SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    bw_sym_ok = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    return TRUE;
}

BW_NOINLINE BW_NO_SANITIZE_ADDRESS size_t bw_win_capture(uintptr_t* ips, size_t max) {
    PVOID frames[BW_WIN_MAX_FRAMES];
    USHORT n = 0;
    USHORT capture = 0;
    USHORT i = 0;

    if (ips == NULL || max == 0) {
        return 0;
    }

    capture = (USHORT)max;
    if (capture > BW_WIN_MAX_FRAMES) {
        capture = BW_WIN_MAX_FRAMES;
    }

    n = CaptureStackBackTrace((ULONG)BW_WIN_SKIP, capture, frames, NULL);
    for (i = 0; i < n; ++i) {
        ips[i] = (uintptr_t)frames[i];
    }

    return (size_t)n;
}

static void bw_copy_cstr(char* dst, size_t dst_len, const char* src) {
    size_t i = 0;

    if (dst == NULL || dst_len == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '?';
        if (dst_len > 1) {
            dst[1] = '\0';
        }
        return;
    }

    while (src[i] != '\0' && i + 1 < dst_len) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void bw_section_name(const IMAGE_SECTION_HEADER* sh,
                            const BYTE* strtab,
                            DWORD str_sz,
                            char* out,
                            size_t out_len) {
    size_t i = 0;
    const char* name = (const char*)sh->Name;
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (sh->Name[0] == '/') {
        DWORD off = 0;
        size_t d = 1;
        while (d < 8 && sh->Name[d] >= '0' && sh->Name[d] <= '9') {
            off = off * 10u + (DWORD)(sh->Name[d] - '0');
            ++d;
        }
        if (strtab != NULL && str_sz > 4 && off < str_sz) {
            name = (const char*)(strtab + off);
        }
    }
    while (i + 1 < out_len && name[i] != '\0' && (name != (const char*)sh->Name || i < 8)) {
        out[i] = name[i];
        ++i;
    }
    out[i] = '\0';
}

static const BYTE* bw_section_data(const BYTE* base,
                                   size_t file_sz,
                                   const IMAGE_SECTION_HEADER* sh,
                                   size_t* len) {
    if (sh->PointerToRawData == 0 || sh->SizeOfRawData == 0) {
        return NULL;
    }
    if ((size_t)sh->PointerToRawData + (size_t)sh->SizeOfRawData > file_sz) {
        return NULL;
    }
    *len = (size_t)sh->SizeOfRawData;
    return base + sh->PointerToRawData;
}

/* DbgHelp reads PDBs. MinGW/GCC writes a COFF symbol table instead. */
enum { BW_COFF_SYM_SIZE = IMAGE_SIZEOF_SYMBOL };
enum { BW_PE_SYM_MAX = 4096 };
enum { BW_PE_NAME_MAX = 256 * 1024 };

typedef struct {
    uintptr_t addr;
    uint32_t name_off;
} bw_pe_sym_t;

static HMODULE bw_pe_mod = NULL;
static int bw_pe_state = 0; /* 0 empty, 1 loaded, -1 none */
static bw_pe_sym_t bw_pe_syms[BW_PE_SYM_MAX];
static size_t bw_pe_nsym = 0;
static char bw_pe_names[BW_PE_NAME_MAX];
static size_t bw_pe_nlen = 0;
static uintptr_t bw_pe_image_base = 0;
static bw_dwarf_tab_t bw_dwarf;
static int bw_dwarf_ready = 0;

static int bw_pe_sym_cmp(const void* a, const void* b) {
    const bw_pe_sym_t* sa = (const bw_pe_sym_t*)a;
    const bw_pe_sym_t* sb = (const bw_pe_sym_t*)b;
    if (sa->addr < sb->addr) {
        return -1;
    }
    if (sa->addr > sb->addr) {
        return 1;
    }
    return 0;
}

static DWORD bw_read_u32(const BYTE* p) {
    DWORD v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int bw_pe_add_name(const char* src, size_t src_len) {
    size_t i = 0;
    if (src == NULL || src_len == 0 || bw_pe_nlen >= BW_PE_NAME_MAX) {
        return -1;
    }
    if (src_len >= BW_PE_NAME_MAX - bw_pe_nlen) {
        src_len = BW_PE_NAME_MAX - bw_pe_nlen - 1;
    }
    if (src_len == 0) {
        return -1;
    }
    while (i < src_len && src[i] != '\0') {
        bw_pe_names[bw_pe_nlen + i] = src[i];
        ++i;
    }
    if (i == 0) {
        return -1;
    }
    bw_pe_names[bw_pe_nlen + i] = '\0';
    {
        int off = (int)bw_pe_nlen;
        bw_pe_nlen += i + 1;
        return off;
    }
}

static void bw_pe_load(HMODULE module, const char* path) {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE map = NULL;
    const BYTE* base = NULL;
    LARGE_INTEGER file_sz_li;
    size_t file_sz = 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_FILE_HEADER fh;
    DWORD e_lfanew = 0;
    DWORD sec_off = 0;
    DWORD sym_off = 0;
    DWORD nsym = 0;
    DWORD str_off = 0;
    DWORD str_sz = 0;
    WORD nsec = 0;
    DWORD i = 0;

    bw_pe_mod = module;
    bw_pe_state = -1;
    bw_pe_nsym = 0;
    bw_pe_nlen = 0;
    bw_pe_image_base = 0;
    bw_dwarf_ready = 0;
    bw_dwarf_tab_reset(&bw_dwarf);

    if (module == NULL || path == NULL || path[0] == '\0' || path[0] == '?') {
        return;
    }

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!GetFileSizeEx(file, &file_sz_li) || file_sz_li.QuadPart <= 0) {
        CloseHandle(file);
        return;
    }
    file_sz = (size_t)file_sz_li.QuadPart;
    map = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (map == NULL) {
        CloseHandle(file);
        return;
    }
    base = (const BYTE*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (base == NULL) {
        CloseHandle(map);
        CloseHandle(file);
        return;
    }

    do {
        if (file_sz < sizeof(IMAGE_DOS_HEADER)) {
            break;
        }
        memcpy(&dos, base, sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            break;
        }
        e_lfanew = (DWORD)dos.e_lfanew;
        if ((size_t)e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) > file_sz) {
            break;
        }
        if (bw_read_u32(base + e_lfanew) != IMAGE_NT_SIGNATURE) {
            break;
        }
        memcpy(&fh, base + e_lfanew + 4, sizeof(fh));
        nsec = fh.NumberOfSections;
        if (nsec == 0 || nsec > 96) {
            break;
        }
        sec_off = e_lfanew + 4 + (DWORD)sizeof(IMAGE_FILE_HEADER) + fh.SizeOfOptionalHeader;
        if ((size_t)sec_off + (size_t)nsec * sizeof(IMAGE_SECTION_HEADER) > file_sz) {
            break;
        }
        sym_off = fh.PointerToSymbolTable;
        nsym = fh.NumberOfSymbols;
        bw_pe_image_base = 0;
        bw_dwarf_ready = 0;
        bw_dwarf_tab_reset(&bw_dwarf);
        {
            const BYTE* opt = base + e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER);
            WORD magic = 0;
            if (fh.SizeOfOptionalHeader >= 28) {
                memcpy(&magic, opt, sizeof(magic));
                if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fh.SizeOfOptionalHeader >= 32) {
                    ULONGLONG ib = 0;
                    memcpy(&ib, opt + 24, sizeof(ib));
                    bw_pe_image_base = (uintptr_t)ib;
                } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
                    DWORD ib = 0;
                    memcpy(&ib, opt + 28, sizeof(ib));
                    bw_pe_image_base = (uintptr_t)ib;
                }
            }
        }

        str_off = 0;
        str_sz = 0;
        if (sym_off != 0 && nsym != 0 &&
            (size_t)sym_off + (size_t)nsym * BW_COFF_SYM_SIZE + 4 <= file_sz) {
            str_off = sym_off + nsym * BW_COFF_SYM_SIZE;
            str_sz = bw_read_u32(base + str_off);
            if (str_sz < 4 || (size_t)str_off + str_sz > file_sz) {
                str_sz = 0;
            }
        }

        if (sym_off != 0 && nsym != 0 &&
            (size_t)sym_off + (size_t)nsym * BW_COFF_SYM_SIZE <= file_sz) {
        for (i = 0; i < nsym && bw_pe_nsym < BW_PE_SYM_MAX; ++i) {
            const BYTE* raw = base + sym_off + i * BW_COFF_SYM_SIZE;
            DWORD value = 0;
            SHORT sec_num = 0;
            WORD type = 0;
            BYTE naux = 0;
            const char* name = NULL;
            size_t name_len = 0;
            int name_off = -1;
            IMAGE_SECTION_HEADER sh;
            uintptr_t addr = 0;

            memcpy(&value, raw + 8, sizeof(value));
            memcpy(&sec_num, raw + 12, sizeof(sec_num));
            memcpy(&type, raw + 14, sizeof(type));
            naux = raw[17];

            if (naux != 0) {
                if (i + naux >= nsym) {
                    break;
                }
                i += naux;
            }

            if (sec_num <= 0 || sec_num > (SHORT)nsec) {
                continue;
            }
            if (!ISFCN(type)) {
                continue;
            }

            memcpy(&sh, base + sec_off + (DWORD)(sec_num - 1) * sizeof(IMAGE_SECTION_HEADER), sizeof(sh));
            addr = (uintptr_t)module + (uintptr_t)sh.VirtualAddress + (uintptr_t)value;

            if (bw_read_u32(raw) == 0) {
                DWORD long_off = bw_read_u32(raw + 4);
                if (str_sz == 0 || long_off >= str_sz) {
                    continue;
                }
                name = (const char*)(base + str_off + long_off);
                name_len = str_sz - long_off;
            } else {
                name = (const char*)raw;
                name_len = 8;
            }
            if (name[0] == '.' || name[0] == '\0') {
                continue;
            }
            name_off = bw_pe_add_name(name, name_len);
            if (name_off < 0) {
                break;
            }
            bw_pe_syms[bw_pe_nsym].addr = addr;
            bw_pe_syms[bw_pe_nsym].name_off = (uint32_t)name_off;
            bw_pe_nsym += 1;
        }
        }

        if (bw_pe_nsym > 0) {
            qsort(bw_pe_syms, bw_pe_nsym, sizeof(bw_pe_syms[0]), bw_pe_sym_cmp);
            bw_pe_state = 1;
        }

        {
            const BYTE* line = NULL;
            const BYTE* line_str = NULL;
            const BYTE* debug_str = NULL;
            size_t line_len = 0;
            size_t line_str_len = 0;
            size_t debug_str_len = 0;
            WORD s = 0;
            const BYTE* strtab = str_sz > 0 ? base + str_off : NULL;
            for (s = 0; s < nsec; ++s) {
                IMAGE_SECTION_HEADER sh;
                char name[64];
                const BYTE* data = NULL;
                size_t len = 0;
                memcpy(&sh, base + sec_off + (DWORD)s * sizeof(IMAGE_SECTION_HEADER), sizeof(sh));
                bw_section_name(&sh, strtab, str_sz, name, sizeof(name));
                data = bw_section_data(base, file_sz, &sh, &len);
                if (data == NULL) {
                    continue;
                }
                if (strcmp(name, ".debug_line") == 0) {
                    line = data;
                    line_len = len;
                } else if (strcmp(name, ".debug_line_str") == 0) {
                    line_str = data;
                    line_str_len = len;
                } else if (strcmp(name, ".debug_str") == 0) {
                    debug_str = data;
                    debug_str_len = len;
                }
            }
            if (line != NULL) {
                bw_dwarf_ready = bw_dwarf_tab_parse(&bw_dwarf, line, line_len, line_str, line_str_len, debug_str,
                                                    debug_str_len)
                                     ? 1
                                     : 0;
            }
        }
    } while (0);

    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(file);
}

static const char* bw_pe_find(HMODULE module, const char* path, uintptr_t ip) {
    size_t lo = 0;
    size_t hi = 0;
    size_t best = (size_t)-1;

    if (module == NULL) {
        return NULL;
    }
    if (bw_pe_mod != module) {
        bw_pe_load(module, path);
    }
    if (bw_pe_state != 1) {
        return NULL;
    }

    hi = bw_pe_nsym;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (bw_pe_syms[mid].addr <= ip) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (best == (size_t)-1) {
        return NULL;
    }
    return bw_pe_names + bw_pe_syms[best].name_off;
}

static void bw_pe_line(HMODULE module,
                       const char* path,
                       uintptr_t probe,
                       char* src,
                       size_t src_len,
                       uint32_t* line) {
    const char* file = NULL;
    uint32_t ln = 0;
    uintptr_t linked = probe;

    if (module == NULL) {
        return;
    }
    if (bw_pe_mod != module) {
        bw_pe_load(module, path);
    }
    if (!bw_dwarf_ready) {
        return;
    }
    if (bw_pe_image_base != 0) {
        linked = bw_pe_image_base + (probe - (uintptr_t)module);
    }
    if (bw_dwarf_tab_lookup(&bw_dwarf, linked, &file, &ln) && file != NULL) {
        bw_copy_cstr(src, src_len, file);
        if (line != NULL) {
            *line = ln;
        }
    }
}

BW_NO_SANITIZE_ADDRESS void bw_win_resolve(uintptr_t ip,
                    uintptr_t* mod_addr,
                    char* fname,
                    size_t fname_len,
                    char* sname,
                    size_t sname_len,
                    char* src,
                    size_t src_len,
                    uint32_t* line) {
    HMODULE module = NULL;
    char pe_path[MAX_PATH];
    char sym_storage[sizeof(SYMBOL_INFO) + BW_WIN_SNAME_MAX];
    SYMBOL_INFO* info = (SYMBOL_INFO*)sym_storage;
    IMAGEHLP_LINE64 ln64;
    DWORD line_disp = 0;
    DWORD64 displacement = 0;
    BOOL ok = FALSE;
    BOOL line_ok = FALSE;
    const char* pe_name = NULL;
    uintptr_t probe = 0;

    if (mod_addr != NULL) {
        *mod_addr = 0;
    }
    if (line != NULL) {
        *line = 0;
    }
    bw_copy_cstr(fname, fname_len, "?");
    bw_copy_cstr(sname, sname_len, "?");
    bw_copy_cstr(src, src_len, "?");
    pe_path[0] = '\0';
    memset(&ln64, 0, sizeof(ln64));
    ln64.SizeOfStruct = sizeof(ln64);

    if (ip == 0) {
        return;
    }

    (void)InitOnceExecuteOnce(&bw_sym_once, bw_sym_init, NULL, NULL);

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           // NOLINTNEXTLINE(performance-no-int-to-ptr)
                           (LPCSTR)(ULONG_PTR)ip,
                           &module) &&
        module != NULL) {
        DWORD n = 0;
        if (mod_addr != NULL) {
            *mod_addr = ip - (uintptr_t)module;
        }
        n = GetModuleFileNameA(module, pe_path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            pe_path[0] = '\0';
        } else {
            bw_copy_cstr(fname, fname_len, pe_path);
        }
    }

    probe = ip > 0 ? ip - 1 : ip;

    EnterCriticalSection(&bw_sym_cs);
    if (bw_sym_ok && memset(sym_storage, 0, sizeof(sym_storage)) == sym_storage) {
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = BW_WIN_SNAME_MAX;
        ok = SymFromAddr(GetCurrentProcess(), (DWORD64)ip, &displacement, info);
        if (!ok) {
            ok = SymFromAddr(GetCurrentProcess(), (DWORD64)probe, &displacement, info);
        }
        line_ok = SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)probe, &line_disp, &ln64);
        if (!line_ok) {
            line_ok = SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)ip, &line_disp, &ln64);
        }
    }
    if (ok && info->Name[0] != '\0') {
        bw_copy_cstr(sname, sname_len, info->Name);
    } else {
        pe_name = bw_pe_find(module, pe_path, probe);
        if (pe_name != NULL) {
#if defined(__i386__) || defined(_M_IX86)
            if (pe_name[0] == '_') {
                pe_name += 1;
            }
#endif
            bw_copy_cstr(sname, sname_len, pe_name);
        }
    }
    if (line_ok && ln64.FileName != NULL && ln64.FileName[0] != '\0') {
        bw_copy_cstr(src, src_len, ln64.FileName);
        if (line != NULL) {
            *line = (uint32_t)ln64.LineNumber;
        }
    } else {
        bw_pe_line(module, pe_path, probe, src, src_len, line);
    }
    LeaveCriticalSection(&bw_sym_cs);
}
