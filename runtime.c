/* gbpython runtime: allocators, value helpers, output window, errors.
   None of this code executes from banked ROM — it lives in always-mapped
   bank 0 so both the UI (bank 0) and the interpreter (bank 1) can call it.
   SRAM banking (SWITCH_RAM) is just an MBC register write and works from
   any ROM bank. */

#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <string.h>
#include "gbpython.h"

/* --- SRAM arenas ---------------------------------------------------------
   Bank 1 holds two AST arenas: the per-run arena grows up from 0xA000 and
   is wiped after every run; def subtrees are allocated downward from 0xC000
   (def_mode is set while parsing a def) so functions survive across runs,
   like a real REPL. Bank 2 = strings, bank 3 = lists/dicts. */

uint8_t* sram_ast_ptr = (uint8_t*)0xA000;
uint8_t* sram_def_ptr = (uint8_t*)0xC000;
uint8_t def_mode = 0;
uint8_t* sram_str_ptr = (uint8_t*)0xA000;
uint8_t* sram_list_ptr = (uint8_t*)0xA000;

void* sram_ast_alloc(size_t size) {
    uint8_t* allocated;
    if (def_mode) {
        allocated = sram_def_ptr - size;
        if (allocated <= sram_ast_ptr) {
            raise_memory_error();
            return NULL;
        }
        sram_def_ptr = allocated;
        return (void*)allocated;
    }
    allocated = sram_ast_ptr;
    if (sram_ast_ptr + size >= sram_def_ptr) {
        raise_memory_error();
        return NULL;
    }
    sram_ast_ptr += size;
    return (void*)allocated;
}

void* sram_str_alloc(size_t size) {
    uint8_t* allocated = sram_str_ptr;
    if (sram_str_ptr + size > (uint8_t*)0xBFFF) {
        raise_memory_error();
        return NULL;
    }
    sram_str_ptr += size;
    return (void*)allocated;
}

void* sram_list_alloc(size_t size) {
    uint8_t* allocated = sram_list_ptr;
    if (sram_list_ptr + size > (uint8_t*)0xBFFF) {
        raise_memory_error();
        return NULL;
    }
    sram_list_ptr += size;
    return (void*)allocated;
}

void sram_ast_reset(void) {
    sram_ast_ptr = (uint8_t*)0xA000;
}

ASTNode* make_node(ASTNodeType type) {
    /* Caller (run_interpreter) keeps SRAM enabled for the whole parse/eval
       cycle; disabling here would drop the field writes below on real
       hardware. */
    ASTNode* n;
    SWITCH_RAM(1);
    n = (ASTNode*)sram_ast_alloc(sizeof(ASTNode));
    if (n != NULL) {
        n->type = type;
        n->left = NULL;
        n->right = NULL;
    }
    return n;
}

/* --- Value channel -------------------------------------------------------
   Type (and, for strings, SRAM bank) of the value evaluate() just
   produced. */
uint8_t last_eval_type = TYPE_INT;
uint8_t last_eval_str_bank = 2;

/* --- Control-flow signals ------------------------------------------------
   Raised by break/continue/return, consumed by the nearest enclosing loop
   or call. SIG_ERROR aborts the whole run. */
uint8_t exec_signal = SIG_NONE;
int return_value;
uint8_t return_type;
uint8_t return_str_bank;

char err_buf[28];

void raise_error(const char* msg) {
    strcpy(err_buf, msg);
    exec_signal = SIG_ERROR;
}

void raise_error_name(const char* kind, const char* name) {
    sprintf(err_buf, "%s: %s", kind, name);
    exec_signal = SIG_ERROR;
}

void raise_memory_error(void) {
    raise_error("MemoryError");
}

/* --- Strings -------------------------------------------------------------
   Scratch for string binops; bank-2 strings can be concat results (<=32) */
char sbuf_l[33];
char sbuf_r[33];

void fetch_str(char* dst, int val, uint8_t bank) {
    SWITCH_RAM(bank);
    strcpy(dst, (char*)val);
    SWITCH_RAM(1);
}

/* Bank-1 strings live in the AST arena, which is wiped after each run;
   copy them into the persistent bank-2 arena before storing. Bank-2
   pointers are shared as-is (strings are immutable). */
int store_str_value(int val, uint8_t str_bank) {
    char temp[33];
    char* str_copy;
    if (str_bank == 2) return val;
    SWITCH_RAM(1);
    strcpy(temp, (char*)val);
    SWITCH_RAM(2);
    str_copy = (char*)sram_str_alloc(strlen(temp) + 1);
    if (str_copy != NULL) {
        strcpy(str_copy, temp);
    }
    SWITCH_RAM(1);
    return str_copy != NULL ? (int)str_copy : 0;
}

/* Concatenate sbuf_l + sbuf_r (capped at 32 chars) into a new bank-2 string */
int str_concat(void) {
    uint8_t ll = strlen(sbuf_l);
    uint8_t i = 0;
    char* dst;
    while (ll < 32 && sbuf_r[i]) {
        sbuf_l[ll++] = sbuf_r[i++];
    }
    sbuf_l[ll] = '\0';
    SWITCH_RAM(2);
    dst = (char*)sram_str_alloc(ll + 1);
    if (dst != NULL) {
        strcpy(dst, sbuf_l);
    }
    SWITCH_RAM(1);
    last_eval_type = TYPE_STR;
    last_eval_str_bank = 2;
    return dst != NULL ? (int)dst : 0;
}

/* --- Lists ---------------------------------------------------------------
   Bank 3: [int16 len][type u8, value i16]*len. Elements carry their own
   type: ints, bools, strings (bank-2 pointers), None, or nested lists. */

int list_len(int ptr) {
    int v;
    if (!ptr) return 0;
    SWITCH_RAM(3);
    v = *(int*)ptr;
    SWITCH_RAM(1);
    return v;
}

int list_get(int ptr, int i, uint8_t* t) {
    int v;
    int addr = ptr + 2 + 3 * i;
    SWITCH_RAM(3);
    *t = *(uint8_t*)addr;
    v = *(int*)(addr + 1);
    SWITCH_RAM(1);
    return v;
}

void list_set(int ptr, int i, int v, uint8_t t) {
    int addr = ptr + 2 + 3 * i;
    SWITCH_RAM(3);
    *(uint8_t*)addr = t;
    *(int*)(addr + 1) = v;
    SWITCH_RAM(1);
}

int list_new(int len) {
    uint8_t* p;
    SWITCH_RAM(3);
    p = (uint8_t*)sram_list_alloc(2 + 3 * len);
    if (p != NULL) {
        *(int*)p = len;
    }
    SWITCH_RAM(1);
    return p != NULL ? (int)p : 0;
}

/* Structural equality, python-style: [1,'a',[2]] == [1,'a',[2]] */
uint8_t list_eq(int a, int b) {
    int la = list_len(a);
    int i;
    if (la != list_len(b)) return 0;
    for (i = 0; i < la; i++) {
        uint8_t ta, tb;
        int va = list_get(a, i, &ta);
        int vb = list_get(b, i, &tb);
        uint8_t na = (ta == TYPE_INT || ta == TYPE_BOOL);
        uint8_t nb = (tb == TYPE_INT || tb == TYPE_BOOL);
        if (na && nb) {
            if (va != vb) return 0;
        } else if (ta == TYPE_STR && tb == TYPE_STR) {
            char x[33], y[33];
            fetch_str(x, va, 2);
            fetch_str(y, vb, 2);
            if (strcmp(x, y) != 0) return 0;
        } else if (ta == TYPE_LIST && tb == TYPE_LIST) {
            if (!list_eq(va, vb)) return 0;
        } else if (ta == TYPE_NONE && tb == TYPE_NONE) {
            /* equal */
        } else {
            return 0;
        }
    }
    return 1;
}

/* Membership: needle (lv,lt) in list ptr */
uint8_t list_contains(int ptr, int lv, uint8_t lt, uint8_t lbank) {
    int len = list_len(ptr);
    int i;
    for (i = 0; i < len; i++) {
        uint8_t et;
        int ev = list_get(ptr, i, &et);
        uint8_t nl_ = (lt == TYPE_INT || lt == TYPE_BOOL);
        uint8_t ne = (et == TYPE_INT || et == TYPE_BOOL);
        if (nl_ && ne) {
            if (lv == ev) return 1;
        } else if (lt == TYPE_STR && et == TYPE_STR) {
            char x[33], y[33];
            fetch_str(x, lv, lbank);
            fetch_str(y, ev, 2);
            if (strcmp(x, y) == 0) return 1;
        } else if (lt == TYPE_LIST && et == TYPE_LIST) {
            if (list_eq(lv, ev)) return 1;
        }
    }
    return 0;
}

/* --- Dicts ---------------------------------------------------------------
   Bank 3, chained blocks so the head address never changes (python
   aliasing: d2 = d, d2['x'] = 1 must be visible through d).
   Head/block layout: [count i16][next i16][4 entries of
   (ktype u8, kval i16, vtype u8, vval i16) = 6 bytes] = 28 bytes.
   count is only meaningful in the head block. */

#define DICT_BLOCK_ENTRIES 4
#define DICT_BLOCK_SIZE (4 + DICT_BLOCK_ENTRIES * 6)

int dict_new(void) {
    uint8_t* p;
    SWITCH_RAM(3);
    p = (uint8_t*)sram_list_alloc(DICT_BLOCK_SIZE);
    if (p != NULL) {
        *(int*)p = 0;
        *(int*)(p + 2) = 0;
    }
    SWITCH_RAM(1);
    return p != NULL ? (int)p : 0;
}

int dict_len(int d) {
    int v;
    if (!d) return 0;
    SWITCH_RAM(3);
    v = *(int*)d;
    SWITCH_RAM(1);
    return v;
}

/* Address of entry i, walking the block chain. Bank 3 must be active. */
static int dict_entry_addr(int d, int i) {
    while (i >= DICT_BLOCK_ENTRIES) {
        d = *(int*)(d + 2);
        i -= DICT_BLOCK_ENTRIES;
    }
    return d + 4 + 6 * i;
}

void dict_entry(int d, int i, uint8_t* kt, int* kv, uint8_t* vt, int* vv) {
    int e;
    SWITCH_RAM(3);
    e = dict_entry_addr(d, i);
    *kt = *(uint8_t*)e;
    *kv = *(int*)(e + 1);
    *vt = *(uint8_t*)(e + 3);
    *vv = *(int*)(e + 4);
    SWITCH_RAM(1);
}

/* Index of key in dict, or -1. Keys are ints, bools, or strings. */
int dict_find(int d, uint8_t kt, int kv, uint8_t kbank) {
    int n = dict_len(d);
    int i;
    uint8_t et, vt_;
    int ev, vv_;
    for (i = 0; i < n; i++) {
        dict_entry(d, i, &et, &ev, &vt_, &vv_);
        if ((kt == TYPE_INT || kt == TYPE_BOOL) &&
            (et == TYPE_INT || et == TYPE_BOOL)) {
            if (kv == ev) return i;
        } else if (kt == TYPE_STR && et == TYPE_STR) {
            char x[33], y[33];
            fetch_str(x, kv, kbank);
            fetch_str(y, ev, 2);
            if (strcmp(x, y) == 0) return i;
        }
    }
    return -1;
}

/* Set d[key] = value. Key strings must already be in bank 2. Returns 0 on
   allocation failure (error already raised). */
uint8_t dict_set(int d, uint8_t kt, int kv, uint8_t vt, int vv) {
    int i = dict_find(d, kt, kv, 2);
    int e, blk, n;
    if (i >= 0) {
        SWITCH_RAM(3);
        e = dict_entry_addr(d, i);
        *(uint8_t*)(e + 3) = vt;
        *(int*)(e + 4) = vv;
        SWITCH_RAM(1);
        return 1;
    }
    n = dict_len(d);
    SWITCH_RAM(3);
    /* walk to the last block, allocating a new one if it's full */
    blk = d;
    i = n;
    while (i > DICT_BLOCK_ENTRIES - 1 && *(int*)(blk + 2)) {
        blk = *(int*)(blk + 2);
        i -= DICT_BLOCK_ENTRIES;
    }
    if (i >= DICT_BLOCK_ENTRIES) {
        uint8_t* p = (uint8_t*)sram_list_alloc(DICT_BLOCK_SIZE);
        if (p == NULL) {
            SWITCH_RAM(1);
            return 0;
        }
        *(int*)(p + 2) = 0;
        *(int*)(blk + 2) = (int)p;
        blk = (int)p;
        i -= DICT_BLOCK_ENTRIES;
    }
    e = blk + 4 + 6 * i;
    *(uint8_t*)e = kt;
    *(int*)(e + 1) = kv;
    *(uint8_t*)(e + 3) = vt;
    *(int*)(e + 4) = vv;
    *(int*)d = n + 1;
    SWITCH_RAM(1);
    return 1;
}

/* --- Output window -------------------------------------------------------
   Fixed lines under "--- Out ---". Keeping output inside this window
   (instead of raw printf) stops long output from scrolling the gbdk
   console and wrecking the screen layout. */

#define OUT_ROWS 5
char out_lines[OUT_ROWS][21];
uint8_t out_count = 0;

void out_redraw(void) {
    uint8_t i, j;
    for (i = 0; i < OUT_ROWS; i++) {
        gotoxy(0, 7 + i);
        j = 0;
        if (i < out_count) {
            while (out_lines[i][j]) {
                putchar(out_lines[i][j]);
                j++;
            }
        }
        while (j < 20) {
            putchar(' ');
            j++;
        }
    }
}

void out_putline(const char* s) {
    uint8_t i;
    if (out_count == OUT_ROWS) {
        for (i = 0; i < OUT_ROWS - 1; i++) {
            strcpy(out_lines[i], out_lines[i + 1]);
        }
        out_count = OUT_ROWS - 1;
    }
    for (i = 0; i < 20 && s[i]; i++) {
        out_lines[out_count][i] = s[i];
    }
    out_lines[out_count][i] = '\0';
    out_count++;
    out_redraw();
}

/* --- Value rendering -----------------------------------------------------
   Render (possibly nested) values as python would, truncating with ".."
   once the 20-column screen line is spent. */

char line_buf[24];

void render_val(uint8_t t, int v, char* buf, uint8_t* pos);

void render_list_inner(int ptr, char* buf, uint8_t* pos) {
    int len = list_len(ptr);
    int i;
    buf[(*pos)++] = '[';
    for (i = 0; i < len; i++) {
        uint8_t t;
        int v = list_get(ptr, i, &t);
        if (*pos > 14) {
            buf[(*pos)++] = '.';
            buf[(*pos)++] = '.';
            break;
        }
        if (i) {
            buf[(*pos)++] = ',';
            buf[(*pos)++] = ' ';
        }
        render_val(t, v, buf, pos);
    }
    buf[(*pos)++] = ']';
}

void render_dict_inner(int d, char* buf, uint8_t* pos) {
    int len = dict_len(d);
    int i;
    buf[(*pos)++] = '{';
    for (i = 0; i < len; i++) {
        uint8_t kt, vt;
        int kv, vv;
        if (*pos > 12) {
            buf[(*pos)++] = '.';
            buf[(*pos)++] = '.';
            break;
        }
        dict_entry(d, i, &kt, &kv, &vt, &vv);
        if (i) {
            buf[(*pos)++] = ',';
            buf[(*pos)++] = ' ';
        }
        render_val(kt, kv, buf, pos);
        buf[(*pos)++] = ':';
        buf[(*pos)++] = ' ';
        render_val(vt, vv, buf, pos);
    }
    buf[(*pos)++] = '}';
}

void render_val(uint8_t t, int v, char* buf, uint8_t* pos) {
    char tmp[20];
    uint8_t nl;
    if (t == TYPE_LIST || t == TYPE_DICT) {
        if (*pos > 14) {
            buf[(*pos)++] = '.';
            buf[(*pos)++] = '.';
            return;
        }
        if (t == TYPE_LIST) render_list_inner(v, buf, pos);
        else render_dict_inner(v, buf, pos);
        return;
    }
    if (t == TYPE_STR) {
        uint8_t i = 0;
        char* s = (char*)v;
        tmp[i++] = '\'';
        SWITCH_RAM(2); /* list/dict/str elements always live in bank 2 */
        while (*s && i < 16) {
            tmp[i++] = *s++;
        }
        SWITCH_RAM(1);
        tmp[i++] = '\'';
        tmp[i] = '\0';
    } else if (t == TYPE_BOOL) {
        strcpy(tmp, v ? "True" : "False");
    } else if (t == TYPE_NONE) {
        strcpy(tmp, "None");
    } else {
        sprintf(tmp, "%d", v);
    }
    nl = strlen(tmp);
    if (*pos + nl > 17) {
        buf[(*pos)++] = '.';
        buf[(*pos)++] = '.';
        return;
    }
    strcpy(buf + *pos, tmp);
    *pos += nl;
}

void render_list(int ptr, char* buf) {
    uint8_t pos = 0;
    render_list_inner(ptr, buf, &pos);
    buf[pos] = '\0';
}

/* Copy a string value out of banked SRAM, then emit it on the output
   window. echo=1 gives the REPL "> 'x'" form, echo=0 the print() form. */
void emit_value(int val, uint8_t vtype, uint8_t str_bank, uint8_t echo) {
    if (vtype == TYPE_STR) {
        char temp[33];
        SWITCH_RAM(str_bank);
        strcpy(temp, (char*)val);
        SWITCH_RAM(1);
        if (echo) {
            sprintf(line_buf, "> '%s'", temp);
        } else {
            sprintf(line_buf, "%s", temp);
        }
    } else if (vtype == TYPE_LIST || vtype == TYPE_DICT) {
        char temp[24];
        uint8_t pos = 0;
        if (vtype == TYPE_LIST) render_list_inner(val, temp, &pos);
        else render_dict_inner(val, temp, &pos);
        temp[pos] = '\0';
        if (echo) {
            sprintf(line_buf, "> %s", temp);
        } else {
            strcpy(line_buf, temp);
        }
    } else if (vtype == TYPE_BOOL) {
        sprintf(line_buf, echo ? "> %s" : "%s", val ? "True" : "False");
    } else if (vtype == TYPE_NONE) {
        sprintf(line_buf, echo ? "> %s" : "%s", "None");
    } else {
        if (echo) {
            sprintf(line_buf, "> %d", val);
        } else {
            sprintf(line_buf, "%d", val);
        }
    }
    out_putline(line_buf);
}

/* Python truthiness: empty strings, lists and dicts are falsy, None is
   falsy */
uint8_t truthy(int val, uint8_t vtype, uint8_t str_bank) {
    char c;
    switch (vtype) {
        case TYPE_STR:
            SWITCH_RAM(str_bank);
            c = *(char*)val;
            SWITCH_RAM(1);
            return c != '\0';
        case TYPE_LIST:
            return list_len(val) > 0;
        case TYPE_DICT:
            return dict_len(val) > 0;
        case TYPE_NONE:
            return 0;
        default:
            return val != 0;
    }
}
