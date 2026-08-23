#ifndef BW_WIN_H
#define BW_WIN_H

#include <stddef.h>
#include <stdint.h>

enum { BW_WIN_MAX_FRAMES = 64 };
enum { BW_WIN_FNAME_MAX = 260 };
enum { BW_WIN_SNAME_MAX = 512 };

size_t bw_win_capture(uintptr_t* ips, size_t max);

void bw_win_resolve(uintptr_t ip,
                    uintptr_t* mod_addr,
                    char* fname,
                    size_t fname_len,
                    char* sname,
                    size_t sname_len);

#endif // BW_WIN_H
