#ifndef BW_DWARF_LINE_H
#define BW_DWARF_LINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { BW_DWARF_ROW_MAX = 65536 };
enum { BW_DWARF_STR_MAX = 256 * 1024 };

typedef struct {
    uintptr_t addr;
    uint32_t name_off;
    uint32_t line;
} bw_dwarf_row_t;

typedef struct {
    bw_dwarf_row_t rows[BW_DWARF_ROW_MAX];
    size_t nrow;
    char strs[BW_DWARF_STR_MAX];
    size_t nstr;
} bw_dwarf_tab_t;

void bw_dwarf_tab_reset(bw_dwarf_tab_t* tab);

bool bw_dwarf_tab_parse(bw_dwarf_tab_t* tab,
                        const unsigned char* line,
                        size_t line_len,
                        const unsigned char* line_str,
                        size_t line_str_len,
                        const unsigned char* debug_str,
                        size_t debug_str_len);

bool bw_dwarf_tab_lookup(const bw_dwarf_tab_t* tab,
                         uintptr_t addr,
                         const char** file,
                         uint32_t* line);

#endif
