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
long return_value;
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
   Scratch for string binops; strings cap at STR_MAX (127) chars */
char sbuf_l[STR_MAX + 1];
char sbuf_r[STR_MAX + 1];

void fetch_str(char* dst, long val, uint8_t bank) {
    SWITCH_RAM(bank);
    strcpy(dst, (char*)(uint16_t)val);
    SWITCH_RAM(1);
}

/* value (int/bool/float) -> float bits */
long num_to_f32(long v, uint8_t t) {
    if (t == TYPE_FLOAT) return v;
    return f32_from_int(v);
}

/* Bank-1 strings live in the AST arena, which is wiped after each run;
   copy them into the persistent bank-2 arena before storing. Bank-2
   pointers are shared as-is (strings are immutable). */
long store_str_value(long val, uint8_t str_bank) {
    char temp[STR_MAX + 1];
    char* str_copy;
    if (str_bank == 2) return val;
    SWITCH_RAM(1);
    strcpy(temp, (char*)(uint16_t)val);
    SWITCH_RAM(2);
    str_copy = (char*)sram_str_alloc(strlen(temp) + 1);
    if (str_copy != NULL) {
        strcpy(str_copy, temp);
    }
    SWITCH_RAM(1);
    return str_copy != NULL ? (long)(uint16_t)str_copy : 0;
}

/* Concatenate sbuf_l + sbuf_r (capped at STR_MAX) into a new bank-2 string */
long str_concat(void) {
    uint8_t ll = strlen(sbuf_l);
    uint8_t i = 0;
    char* dst;
    while (ll < STR_MAX && sbuf_r[i]) {
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
    return dst != NULL ? (long)(uint16_t)dst : 0;
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

long list_get(int ptr, int i, uint8_t* t) {
    long v;
    int addr = ptr + 2 + 5 * i;
    SWITCH_RAM(3);
    *t = *(uint8_t*)addr;
    v = *(long*)(addr + 1);
    SWITCH_RAM(1);
    return v;
}

void list_set(int ptr, int i, long v, uint8_t t) {
    int addr = ptr + 2 + 5 * i;
    SWITCH_RAM(3);
    *(uint8_t*)addr = t;
    *(long*)(addr + 1) = v;
    SWITCH_RAM(1);
}

int list_new(int len) {
    uint8_t* p;
    SWITCH_RAM(3);
    p = (uint8_t*)sram_list_alloc(2 + 5 * len);
    if (p != NULL) {
        *(int*)p = len;
    }
    SWITCH_RAM(1);
    return p != NULL ? (int)p : 0;
}

/* --- Dicts ---------------------------------------------------------------
   Bank 3, chained blocks so the head address never changes (python
   aliasing: d2 = d, d2['x'] = 1 must be visible through d).
   Head/block layout: [count i16][next i16][4 entries of
   (ktype u8, kval i16, vtype u8, vval i16) = 6 bytes] = 28 bytes.
   count is only meaningful in the head block. */

#define DICT_BLOCK_ENTRIES 4
#define DICT_ENTRY_SIZE 10 /* ktype u8, kval i32, vtype u8, vval i32 */
#define DICT_BLOCK_SIZE (4 + DICT_BLOCK_ENTRIES * DICT_ENTRY_SIZE)

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
    return d + 4 + DICT_ENTRY_SIZE * i;
}

void dict_entry(int d, int i, uint8_t* kt, long* kv, uint8_t* vt, long* vv) {
    int e;
    SWITCH_RAM(3);
    e = dict_entry_addr(d, i);
    *kt = *(uint8_t*)e;
    *kv = *(long*)(e + 1);
    *vt = *(uint8_t*)(e + 5);
    *vv = *(long*)(e + 6);
    SWITCH_RAM(1);
}

/* Index of key in dict, or -1. Keys are ints, bools, or strings. */
int dict_find(int d, uint8_t kt, long kv, uint8_t kbank) {
    int n = dict_len(d);
    int i;
    uint8_t et, vt_;
    long ev, vv_;
    for (i = 0; i < n; i++) {
        dict_entry(d, i, &et, &ev, &vt_, &vv_);
        if ((kt == TYPE_INT || kt == TYPE_BOOL) &&
            (et == TYPE_INT || et == TYPE_BOOL)) {
            if (kv == ev) return i;
        } else if (kt == TYPE_STR && et == TYPE_STR) {
            char x[STR_MAX + 1], y[STR_MAX + 1];
            fetch_str(x, kv, kbank);
            fetch_str(y, ev, 2);
            if (strcmp(x, y) == 0) return i;
        }
    }
    return -1;
}

/* Set d[key] = value. Key strings must already be in bank 2. Returns 0 on
   allocation failure (error already raised). */
uint8_t dict_set(int d, uint8_t kt, long kv, uint8_t vt, long vv) {
    int i = dict_find(d, kt, kv, 2);
    int e, blk, n;
    if (i >= 0) {
        SWITCH_RAM(3);
        e = dict_entry_addr(d, i);
        *(uint8_t*)(e + 5) = vt;
        *(long*)(e + 6) = vv;
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
    e = blk + 4 + DICT_ENTRY_SIZE * i;
    *(uint8_t*)e = kt;
    *(long*)(e + 1) = kv;
    *(uint8_t*)(e + 5) = vt;
    *(long*)(e + 6) = vv;
    *(int*)d = n + 1;
    SWITCH_RAM(1);
    return 1;
}

/* Python truthiness: empty strings, lists and dicts are falsy, None is
   falsy */
uint8_t truthy(long val, uint8_t vtype, uint8_t str_bank) {
    char c;
    switch (vtype) {
        case TYPE_STR:
            SWITCH_RAM(str_bank);
            c = *(char*)(uint16_t)val;
            SWITCH_RAM(1);
            return c != '\0';
        case TYPE_LIST:
        case TYPE_TUPLE:
            return list_len((int)val) > 0;
        case TYPE_DICT:
        case TYPE_SET:
            return dict_len((int)val) > 0;
        case TYPE_FLOAT:
            return !f32_is_zero(val);
        case TYPE_OBJ:
            return 1;
        case TYPE_NONE:
            return 0;
        default:
            return val != 0;
    }
}
