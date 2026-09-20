#include "dwarf_line.h"

#include <stdlib.h>
#include <string.h>

enum {
    BW_FORM_ADDR = 0x01,
    BW_FORM_BLOCK2 = 0x03,
    BW_FORM_BLOCK4 = 0x04,
    BW_FORM_DATA2 = 0x05,
    BW_FORM_DATA4 = 0x06,
    BW_FORM_DATA8 = 0x07,
    BW_FORM_STRING = 0x08,
    BW_FORM_BLOCK = 0x09,
    BW_FORM_BLOCK1 = 0x0a,
    BW_FORM_DATA1 = 0x0b,
    BW_FORM_FLAG = 0x0c,
    BW_FORM_SDATA = 0x0d,
    BW_FORM_STRP = 0x0e,
    BW_FORM_UDATA = 0x0f,
    BW_FORM_REF_ADDR = 0x10,
    BW_FORM_SEC_OFFSET = 0x17,
    BW_FORM_EXPRLOC = 0x18,
    BW_FORM_FLAG_PRESENT = 0x19,
    BW_FORM_DATA16 = 0x1e,
    BW_FORM_LINE_STRP = 0x1f,
    BW_FORM_IMPLICIT_CONST = 0x21,
    BW_LNCT_PATH = 1,
    BW_LNCT_DIRECTORY_INDEX = 2,
    BW_LNS_COPY = 1,
    BW_LNS_ADVANCE_PC = 2,
    BW_LNS_ADVANCE_LINE = 3,
    BW_LNS_SET_FILE = 4,
    BW_LNS_SET_COLUMN = 5,
    BW_LNS_NEGATE_STMT = 6,
    BW_LNS_SET_BASIC_BLOCK = 7,
    BW_LNS_CONST_ADD_PC = 8,
    BW_LNS_FIXED_ADVANCE_PC = 9,
    BW_LNE_END_SEQUENCE = 1,
    BW_LNE_SET_ADDRESS = 2,
    BW_LNE_DEFINE_FILE = 3
};

enum { BW_DWARF_FILES_MAX = 512 };
enum { BW_DWARF_DIRS_MAX = 256 };
enum { BW_DWARF_FMT_MAX = 16 };

typedef struct {
    const unsigned char* p;
    const unsigned char* end;
    int dwarf64;
} bw_cur_t;

static int bw_cur_ok(const bw_cur_t* c, size_t n) {
    return c->p != NULL && c->end != NULL && (size_t)(c->end - c->p) >= n;
}

static int bw_u8(bw_cur_t* c, unsigned char* out) {
    if (!bw_cur_ok(c, 1)) {
        return 0;
    }
    *out = *c->p++;
    return 1;
}

static int bw_u16(bw_cur_t* c, uint16_t* out) {
    if (!bw_cur_ok(c, 2)) {
        return 0;
    }
    memcpy(out, c->p, 2);
    c->p += 2;
    return 1;
}

static int bw_u32(bw_cur_t* c, uint32_t* out) {
    if (!bw_cur_ok(c, 4)) {
        return 0;
    }
    memcpy(out, c->p, 4);
    c->p += 4;
    return 1;
}

static int bw_u64(bw_cur_t* c, uint64_t* out) {
    if (!bw_cur_ok(c, 8)) {
        return 0;
    }
    memcpy(out, c->p, 8);
    c->p += 8;
    return 1;
}

static int bw_uleb(bw_cur_t* c, uint64_t* out) {
    uint64_t r = 0;
    unsigned shift = 0;
    while (bw_cur_ok(c, 1)) {
        unsigned char b = *c->p++;
        r |= (uint64_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *out = r;
            return 1;
        }
        shift += 7;
        if (shift >= 64) {
            return 0;
        }
    }
    return 0;
}

static int bw_sleb(bw_cur_t* c, int64_t* out) {
    uint64_t r = 0;
    unsigned shift = 0;
    unsigned char b = 0;
    do {
        if (!bw_cur_ok(c, 1) || shift >= 64) {
            return 0;
        }
        b = *c->p++;
        r |= (uint64_t)(b & 0x7fu) << shift;
        shift += 7;
    } while ((b & 0x80u) != 0);
    if (shift < 64 && (b & 0x40u) != 0) {
        r |= (~(uint64_t)0) << shift;
    }
    *out = (int64_t)r;
    return 1;
}

static int bw_skip(bw_cur_t* c, size_t n) {
    if (!bw_cur_ok(c, n)) {
        return 0;
    }
    c->p += n;
    return 1;
}

static int bw_add_str(bw_dwarf_tab_t* tab, const char* s, size_t n) {
    size_t i = 0;
    if (s == NULL || tab->nstr + 1 > BW_DWARF_STR_MAX) {
        return -1;
    }
    if (n >= BW_DWARF_STR_MAX - tab->nstr) {
        n = BW_DWARF_STR_MAX - tab->nstr - 1;
    }
    while (i < n && s[i] != '\0') {
        tab->strs[tab->nstr + i] = s[i];
        ++i;
    }
    tab->strs[tab->nstr + i] = '\0';
    {
        int off = (int)tab->nstr;
        tab->nstr += i + 1;
        return off;
    }
}

static int bw_cstr_off(const unsigned char* sec, size_t sec_len, uint64_t off) {
    if (sec == NULL || off >= sec_len) {
        return 0;
    }
    return 1;
}

static int bw_form_skip(bw_cur_t* c, uint64_t form, int offset_size) {
    uint64_t n = 0;
    int64_t s = 0;
    switch (form) {
    case BW_FORM_FLAG_PRESENT:
        return 1;
    case BW_FORM_DATA1:
    case BW_FORM_FLAG:
        return bw_skip(c, 1);
    case BW_FORM_DATA2:
        return bw_skip(c, 2);
    case BW_FORM_DATA4:
        return bw_skip(c, 4);
    case BW_FORM_STRP:
    case BW_FORM_LINE_STRP:
    case BW_FORM_SEC_OFFSET:
        return bw_skip(c, (size_t)offset_size);
    case BW_FORM_DATA8:
    case BW_FORM_ADDR:
        return bw_skip(c, 8);
    case BW_FORM_DATA16:
        return bw_skip(c, 16);
    case BW_FORM_UDATA:
        return bw_uleb(c, &n);
    case BW_FORM_SDATA:
        return bw_sleb(c, &s);
    case BW_FORM_STRING:
        while (bw_cur_ok(c, 1)) {
            if (*c->p++ == 0) {
                return 1;
            }
        }
        return 0;
    case BW_FORM_BLOCK1: {
        unsigned char len = 0;
        if (!bw_u8(c, &len)) {
            return 0;
        }
        return bw_skip(c, len);
    }
    case BW_FORM_BLOCK2: {
        uint16_t len = 0;
        if (!bw_u16(c, &len)) {
            return 0;
        }
        return bw_skip(c, len);
    }
    case BW_FORM_BLOCK4: {
        uint32_t len = 0;
        if (!bw_u32(c, &len)) {
            return 0;
        }
        return bw_skip(c, len);
    }
    case BW_FORM_BLOCK:
    case BW_FORM_EXPRLOC:
        if (!bw_uleb(c, &n)) {
            return 0;
        }
        return bw_skip(c, (size_t)n);
    case BW_FORM_IMPLICIT_CONST:
        return 1;
    default:
        if (form == BW_FORM_REF_ADDR) {
            return bw_skip(c, (size_t)offset_size);
        }
        return 0;
    }
}

static int bw_form_path(bw_cur_t* c,
                        uint64_t form,
                        int offset_size,
                        const unsigned char* line_str,
                        size_t line_str_len,
                        const unsigned char* debug_str,
                        size_t debug_str_len,
                        const char** out,
                        size_t* out_len) {
    *out = "";
    *out_len = 0;
    if (form == BW_FORM_STRING) {
        const unsigned char* start = c->p;
        while (bw_cur_ok(c, 1) && *c->p != 0) {
            c->p++;
        }
        if (!bw_cur_ok(c, 1)) {
            return 0;
        }
        *out = (const char*)start;
        *out_len = (size_t)(c->p - start);
        c->p++;
        return 1;
    }
    if (form == BW_FORM_LINE_STRP || form == BW_FORM_STRP) {
        uint64_t off = 0;
        const unsigned char* sec = form == BW_FORM_LINE_STRP ? line_str : debug_str;
        size_t sec_len = form == BW_FORM_LINE_STRP ? line_str_len : debug_str_len;
        if (offset_size == 8) {
            if (!bw_u64(c, &off)) {
                return 0;
            }
        } else {
            uint32_t o32 = 0;
            if (!bw_u32(c, &o32)) {
                return 0;
            }
            off = o32;
        }
        if (!bw_cstr_off(sec, sec_len, off)) {
            return 1;
        }
        *out = (const char*)(sec + off);
        *out_len = strlen(*out);
        return 1;
    }
    return bw_form_skip(c, form, offset_size);
}

static int bw_form_udata(bw_cur_t* c, uint64_t form, uint64_t* out) {
    unsigned char b = 0;
    uint16_t w = 0;
    uint32_t d = 0;
    *out = 0;
    switch (form) {
    case BW_FORM_UDATA:
        return bw_uleb(c, out);
    case BW_FORM_DATA1:
        if (!bw_u8(c, &b)) {
            return 0;
        }
        *out = b;
        return 1;
    case BW_FORM_DATA2:
        if (!bw_u16(c, &w)) {
            return 0;
        }
        *out = w;
        return 1;
    case BW_FORM_DATA4:
        if (!bw_u32(c, &d)) {
            return 0;
        }
        *out = d;
        return 1;
    default:
        return 0;
    }
}

static int bw_join_path(char* dst, size_t dst_len, const char* dir, const char* file) {
    size_t i = 0;
    size_t j = 0;
    int abs_file = 0;
    if (dst_len == 0) {
        return 0;
    }
    if (file == NULL) {
        file = "";
    }
    if (file[0] == '/' || file[0] == '\\' ||
        (((file[0] >= 'A' && file[0] <= 'Z') || (file[0] >= 'a' && file[0] <= 'z')) && file[1] == ':')) {
        abs_file = 1;
    }
    if (!abs_file && dir != NULL && dir[0] != '\0') {
        while (dir[i] != '\0' && i + 1 < dst_len) {
            dst[i] = dir[i];
            ++i;
        }
        if (i > 0 && dst[i - 1] != '/' && dst[i - 1] != '\\' && i + 1 < dst_len) {
            dst[i++] = '/';
        }
    }
    while (file[j] != '\0' && i + 1 < dst_len) {
        dst[i++] = file[j++];
    }
    dst[i] = '\0';
    return i > 0;
}

static int bw_row_cmp(const void* a, const void* b) {
    const bw_dwarf_row_t* ra = (const bw_dwarf_row_t*)a;
    const bw_dwarf_row_t* rb = (const bw_dwarf_row_t*)b;
    if (ra->addr < rb->addr) {
        return -1;
    }
    if (ra->addr > rb->addr) {
        return 1;
    }
    return 0;
}

static void bw_emit(bw_dwarf_tab_t* tab, uintptr_t addr, int file_off, uint64_t line) {
    if (tab->nrow >= BW_DWARF_ROW_MAX || file_off < 0 || addr == 0 || line == 0 || line > 0xffffffffu) {
        return;
    }
    tab->rows[tab->nrow].addr = addr;
    tab->rows[tab->nrow].name_off = (uint32_t)file_off;
    tab->rows[tab->nrow].line = (uint32_t)line;
    tab->nrow += 1;
}

void bw_dwarf_tab_reset(bw_dwarf_tab_t* tab) {
    tab->nrow = 0;
    tab->nstr = 0;
}

bool bw_dwarf_tab_lookup(const bw_dwarf_tab_t* tab, uintptr_t addr, const char** file, uint32_t* line) {
    size_t lo = 0;
    size_t hi = 0;
    size_t best = (size_t)-1;
    if (tab == NULL || tab->nrow == 0) {
        return false;
    }
    hi = tab->nrow;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tab->rows[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (best == (size_t)-1) {
        return false;
    }
    if (file != NULL) {
        *file = tab->strs + tab->rows[best].name_off;
    }
    if (line != NULL) {
        *line = tab->rows[best].line;
    }
    return true;
}

static int bw_parse_unit(bw_dwarf_tab_t* tab,
                         bw_cur_t* c,
                         const unsigned char* line_str,
                         size_t line_str_len,
                         const unsigned char* debug_str,
                         size_t debug_str_len) {
    uint32_t unit32 = 0;
    uint64_t unit_len = 0;
    uint16_t version = 0;
    unsigned char addr_size = 8;
    unsigned char seg_size = 0;
    uint64_t header_len = 0;
    unsigned char min_inst = 1;
    unsigned char max_ops = 1;
    unsigned char def_stmt = 1;
    unsigned char line_range = 1;
    unsigned char opcode_base = 1;
    signed char line_base = 0;
    unsigned char std_len[256];
    const unsigned char* header_len_after = NULL;
    const unsigned char* prog = NULL;
    const unsigned char* unit_end = NULL;
    int offset_size = 4;
    int dir_off[BW_DWARF_DIRS_MAX];
    int file_off[BW_DWARF_FILES_MAX];
    size_t ndir = 0;
    size_t nfile = 0;
    size_t i = 0;
    uintptr_t address = 0;
    uint64_t op_index = 0;
    uint64_t file_idx = 1;
    uint64_t line = 1;
    uint64_t is_stmt = 0;

    memset(std_len, 0, sizeof(std_len));
    memset(dir_off, 0xff, sizeof(dir_off));
    memset(file_off, 0xff, sizeof(file_off));

    if (!bw_u32(c, &unit32)) {
        return 0;
    }
    if (unit32 == 0xffffffffu) {
        if (!bw_u64(c, &unit_len)) {
            return 0;
        }
        c->dwarf64 = 1;
        offset_size = 8;
    } else {
        unit_len = unit32;
        c->dwarf64 = 0;
        offset_size = 4;
    }
    if (unit_len < 2 || !bw_cur_ok(c, (size_t)unit_len)) {
        return 0;
    }
    unit_end = c->p + (size_t)unit_len;
    if (!bw_u16(c, &version)) {
        return 0;
    }
    if (version == 5) {
        if (!bw_u8(c, &addr_size) || !bw_u8(c, &seg_size)) {
            return 0;
        }
        if (seg_size != 0) {
            c->p = unit_end;
            return 1;
        }
    }
    if (c->dwarf64) {
        if (!bw_u64(c, &header_len)) {
            return 0;
        }
    } else {
        uint32_t hl = 0;
        if (!bw_u32(c, &hl)) {
            return 0;
        }
        header_len = hl;
    }
    header_len_after = c->p;
    prog = header_len_after + (size_t)header_len;
    if (prog > unit_end) {
        return 0;
    }

    if (!bw_u8(c, &min_inst)) {
        return 0;
    }
    if (version >= 4) {
        if (!bw_u8(c, &max_ops)) {
            return 0;
        }
    }
    if (min_inst == 0 || max_ops == 0) {
        c->p = unit_end;
        return 1;
    }
    if (!bw_u8(c, &def_stmt)) {
        return 0;
    }
    {
        unsigned char lb = 0;
        if (!bw_u8(c, &lb)) {
            return 0;
        }
        line_base = (signed char)lb;
    }
    if (!bw_u8(c, &line_range) || line_range == 0 || !bw_u8(c, &opcode_base) || opcode_base == 0) {
        return 0;
    }
    for (i = 1; i < opcode_base; ++i) {
        if (!bw_u8(c, &std_len[i])) {
            return 0;
        }
    }

    if (version >= 5) {
        unsigned char fmt_count = 0;
        uint64_t lnct[BW_DWARF_FMT_MAX];
        uint64_t form[BW_DWARF_FMT_MAX];
        uint64_t count = 0;
        if (!bw_u8(c, &fmt_count) || fmt_count > BW_DWARF_FMT_MAX) {
            return 0;
        }
        for (i = 0; i < fmt_count; ++i) {
            if (!bw_uleb(c, &lnct[i]) || !bw_uleb(c, &form[i])) {
                return 0;
            }
        }
        if (!bw_uleb(c, &count) || count > BW_DWARF_DIRS_MAX) {
            return 0;
        }
        for (i = 0; i < count; ++i) {
            const char* path = "";
            size_t path_len = 0;
            size_t f = 0;
            for (f = 0; f < fmt_count; ++f) {
                if (lnct[f] == BW_LNCT_PATH) {
                    if (!bw_form_path(c, form[f], offset_size, line_str, line_str_len, debug_str, debug_str_len, &path,
                                      &path_len)) {
                        return 0;
                    }
                } else if (!bw_form_skip(c, form[f], offset_size)) {
                    return 0;
                }
            }
            dir_off[i] = bw_add_str(tab, path, path_len);
            ndir += 1;
        }
        if (!bw_u8(c, &fmt_count) || fmt_count > BW_DWARF_FMT_MAX) {
            return 0;
        }
        for (i = 0; i < fmt_count; ++i) {
            if (!bw_uleb(c, &lnct[i]) || !bw_uleb(c, &form[i])) {
                return 0;
            }
        }
        if (!bw_uleb(c, &count) || count > BW_DWARF_FILES_MAX) {
            return 0;
        }
        for (i = 0; i < count; ++i) {
            const char* path = "";
            size_t path_len = 0;
            uint64_t dir_idx = 0;
            int have_dir = 0;
            size_t f = 0;
            char joined[512];
            for (f = 0; f < fmt_count; ++f) {
                if (lnct[f] == BW_LNCT_PATH) {
                    if (!bw_form_path(c, form[f], offset_size, line_str, line_str_len, debug_str, debug_str_len, &path,
                                      &path_len)) {
                        return 0;
                    }
                } else if (lnct[f] == BW_LNCT_DIRECTORY_INDEX) {
                    if (!bw_form_udata(c, form[f], &dir_idx)) {
                        return 0;
                    }
                    have_dir = 1;
                } else if (!bw_form_skip(c, form[f], offset_size)) {
                    return 0;
                }
            }
            joined[0] = '\0';
            if (have_dir && dir_idx < ndir && dir_off[dir_idx] >= 0) {
                if (!bw_join_path(joined, sizeof(joined), tab->strs + (size_t)dir_off[dir_idx], path)) {
                    return 0;
                }
                file_off[i] = bw_add_str(tab, joined, strlen(joined));
            } else {
                file_off[i] = bw_add_str(tab, path, path_len);
            }
            nfile += 1;
        }
    } else {
        ndir = 1;
        dir_off[0] = bw_add_str(tab, "", 0);
        while (bw_cur_ok(c, 1) && *c->p != 0 && ndir < BW_DWARF_DIRS_MAX) {
            const char* p = (const char*)c->p;
            size_t n = strlen(p);
            dir_off[ndir] = bw_add_str(tab, p, n);
            c->p += n + 1;
            ndir += 1;
        }
        if (!bw_skip(c, 1)) {
            return 0;
        }
        nfile = 1;
        file_off[0] = -1;
        while (bw_cur_ok(c, 1) && *c->p != 0 && nfile < BW_DWARF_FILES_MAX) {
            const char* p = (const char*)c->p;
            size_t n = strlen(p);
            uint64_t dir_idx = 0;
            uint64_t tmp = 0;
            char joined[512];
            c->p += n + 1;
            if (!bw_uleb(c, &dir_idx) || !bw_uleb(c, &tmp) || !bw_uleb(c, &tmp)) {
                return 0;
            }
            joined[0] = '\0';
            if (dir_idx < ndir && dir_off[dir_idx] >= 0) {
                if (!bw_join_path(joined, sizeof(joined), tab->strs + (size_t)dir_off[dir_idx], p)) {
                    return 0;
                }
                file_off[nfile] = bw_add_str(tab, joined, strlen(joined));
            } else {
                file_off[nfile] = bw_add_str(tab, p, n);
            }
            nfile += 1;
        }
        if (!bw_skip(c, 1)) {
            return 0;
        }
    }

    c->p = prog;
    is_stmt = def_stmt;
    address = 0;
    op_index = 0;
    file_idx = version >= 5 ? 1 : 1;
    line = 1;

    while (c->p < unit_end) {
        unsigned char op = 0;
        if (!bw_u8(c, &op)) {
            break;
        }
        if (op == 0) {
            uint64_t insn_len = 0;
            unsigned char ext = 0;
            const unsigned char* insn_end = NULL;
            if (!bw_uleb(c, &insn_len) || insn_len == 0 || !bw_cur_ok(c, (size_t)insn_len)) {
                break;
            }
            insn_end = c->p + (size_t)insn_len;
            if (!bw_u8(c, &ext)) {
                break;
            }
            if (ext == BW_LNE_END_SEQUENCE) {
                address = 0;
                op_index = 0;
                file_idx = 1;
                line = 1;
                is_stmt = def_stmt;
            } else if (ext == BW_LNE_SET_ADDRESS) {
                uint64_t addr = 0;
                if (addr_size == 8) {
                    if (!bw_u64(c, &addr)) {
                        break;
                    }
                } else if (addr_size == 4) {
                    uint32_t a32 = 0;
                    if (!bw_u32(c, &a32)) {
                        break;
                    }
                    addr = a32;
                } else {
                    break;
                }
                address = (uintptr_t)addr;
                op_index = 0;
            }
            c->p = insn_end;
            continue;
        }
        if (op >= opcode_base) {
            uint64_t adjusted = (uint64_t)(op - opcode_base);
            uint64_t op_advance = adjusted / line_range;
            line = (uint64_t)((int64_t)line + (int64_t)line_base + (int64_t)(adjusted % line_range));
            address += (uintptr_t)min_inst * (uintptr_t)((op_index + op_advance) / max_ops);
            op_index = (op_index + op_advance) % max_ops;
            if (file_idx < nfile) {
                bw_emit(tab, address, file_off[file_idx], line);
            }
            continue;
        }
        if (op == BW_LNS_COPY) {
            if (file_idx < nfile) {
                bw_emit(tab, address, file_off[file_idx], line);
            }
        } else if (op == BW_LNS_ADVANCE_PC) {
            uint64_t op_advance = 0;
            if (!bw_uleb(c, &op_advance)) {
                break;
            }
            address += (uintptr_t)min_inst * (uintptr_t)((op_index + op_advance) / max_ops);
            op_index = (op_index + op_advance) % max_ops;
        } else if (op == BW_LNS_ADVANCE_LINE) {
            int64_t d = 0;
            if (!bw_sleb(c, &d)) {
                break;
            }
            line = (uint64_t)((int64_t)line + d);
        } else if (op == BW_LNS_SET_FILE) {
            if (!bw_uleb(c, &file_idx)) {
                break;
            }
        } else if (op == BW_LNS_CONST_ADD_PC) {
            uint64_t adjusted = (uint64_t)(255 - opcode_base);
            uint64_t op_advance = adjusted / line_range;
            address += (uintptr_t)min_inst * (uintptr_t)((op_index + op_advance) / max_ops);
            op_index = (op_index + op_advance) % max_ops;
        } else if (op == BW_LNS_FIXED_ADVANCE_PC) {
            uint16_t d = 0;
            if (!bw_u16(c, &d)) {
                break;
            }
            address += d;
            op_index = 0;
        } else {
            uint64_t a = 0;
            for (a = 0; a < std_len[op]; ++a) {
                uint64_t dummy = 0;
                if (!bw_uleb(c, &dummy)) {
                    a = std_len[op];
                    break;
                }
            }
        }
        (void)is_stmt;
    }

    c->p = unit_end;
    return 1;
}

bool bw_dwarf_tab_parse(bw_dwarf_tab_t* tab,
                        const unsigned char* line,
                        size_t line_len,
                        const unsigned char* line_str,
                        size_t line_str_len,
                        const unsigned char* debug_str,
                        size_t debug_str_len) {
    bw_cur_t c;
    if (tab == NULL || line == NULL || line_len == 0) {
        return false;
    }
    c.p = line;
    c.end = line + line_len;
    c.dwarf64 = 0;
    while (c.p < c.end) {
        if (!bw_parse_unit(tab, &c, line_str, line_str_len, debug_str, debug_str_len)) {
            break;
        }
    }
    if (tab->nrow == 0) {
        return false;
    }
    qsort(tab->rows, tab->nrow, sizeof(tab->rows[0]), bw_row_cmp);
    return true;
}
