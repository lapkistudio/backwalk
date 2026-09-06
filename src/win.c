#include "win.h"

#include "common.h"  // for BW_NOINLINE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <string.h>

#pragma comment(lib, "dbghelp.lib")

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

BW_NO_SANITIZE_ADDRESS void bw_win_resolve(uintptr_t ip,
                    uintptr_t* mod_addr,
                    char* fname,
                    size_t fname_len,
                    char* sname,
                    size_t sname_len) {
    HMODULE module = NULL;
    char sym_storage[sizeof(SYMBOL_INFO) + BW_WIN_SNAME_MAX];
    SYMBOL_INFO* info = (SYMBOL_INFO*)sym_storage;
    DWORD64 displacement = 0;
    BOOL ok = FALSE;

    if (mod_addr != NULL) {
        *mod_addr = 0;
    }
    bw_copy_cstr(fname, fname_len, "?");
    bw_copy_cstr(sname, sname_len, "?");

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
        if (mod_addr != NULL) {
            *mod_addr = ip - (uintptr_t)module;
        }
        if (fname != NULL && fname_len > 0) {
            DWORD n = GetModuleFileNameA(module, fname, (DWORD)fname_len);
            if (n == 0 || n >= (DWORD)fname_len) {
                bw_copy_cstr(fname, fname_len, "?");
            }
        }
    }

    if (!bw_sym_ok) {
        return;
    }

    if (memset(sym_storage, 0, sizeof(sym_storage)) != sym_storage) {
        return;
    }
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = BW_WIN_SNAME_MAX;

    EnterCriticalSection(&bw_sym_cs);
    ok = SymFromAddr(GetCurrentProcess(), (DWORD64)ip, &displacement, info);
    if (!ok && ip > 0) {
        ok = SymFromAddr(GetCurrentProcess(), (DWORD64)(ip - 1), &displacement, info);
    }
    LeaveCriticalSection(&bw_sym_cs);

    if (!ok || info->Name[0] == '\0') {
        return;
    }

    bw_copy_cstr(sname, sname_len, info->Name);
}
