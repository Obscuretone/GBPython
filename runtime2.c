/* gbpython cold runtime: value rendering, the output window, and deep
   equality. Only runs when printing or comparing containers, so it lives
   in ROM bank 3 behind BANKED calls, keeping bank 0 free for the hot
   allocators and accessors. */

#pragma bank 3

#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gbpython.h"

/* Render like python: "3.5", "4.0", "-2.05" (4 fractional digits max) */
void render_float(long bits, char* buf) BANKED {
    long ip;
    long fr;
    uint8_t neg = 0;
    uint8_t digits = 4;
    uint8_t fl, i;
    char frac[8];
    static const long HALF = 0x3F000000L;   /* 0.5f  */
    if (f32_cmp(bits, 0) < 0) {
        neg = 1;
        bits = f32_neg(bits);
    }
    ip = f32_trunc(bits);
    fr = f32_trunc(f32_add(f32_mul(f32_sub(bits, f32_from_int(ip)),
                                   f32_from_int(10000)),
                           HALF));
    if (fr >= 10000) {
        ip++;
        fr = 0;
    }
    while (digits > 1 && fr % 10 == 0) {
        fr /= 10;
        digits--;
    }
    ltoa(fr, frac, 10);
    fl = strlen(frac);
    buf[0] = '\0';
    if (neg) strcpy(buf, "-");
    ltoa(ip, buf + strlen(buf), 10);
    strcat(buf, ".");
    i = strlen(buf);
    while (fl < digits) {
        buf[i++] = '0';
        digits--;
    }
    strcpy(buf + i, frac);
}

/* Structural equality, python-style: [1,'a',[2]] == [1,'a',[2]] */
/* Numeric-family equality across int/bool/float, python-style (1 == 1.0) */
uint8_t num_eq(uint8_t ta, long va, uint8_t tb, long vb) BANKED {
    if (ta == TYPE_FLOAT || tb == TYPE_FLOAT) {
        return f32_cmp(num_to_f32(va, ta), num_to_f32(vb, tb)) == 0;
    }
    return va == vb;
}

#define IS_NUM(t) ((t) == TYPE_INT || (t) == TYPE_BOOL || (t) == TYPE_FLOAT)

uint8_t list_eq(int a, int b) BANKED {
    int la = list_len(a);
    int i;
    if (la != list_len(b)) return 0;
    for (i = 0; i < la; i++) {
        uint8_t ta, tb;
        long va = list_get(a, i, &ta);
        long vb = list_get(b, i, &tb);
        if (IS_NUM(ta) && IS_NUM(tb)) {
            if (!num_eq(ta, va, tb, vb)) return 0;
        } else if (ta == TYPE_STR && tb == TYPE_STR) {
            char x[STR_MAX + 1], y[STR_MAX + 1];
            fetch_str(x, va, 2);
            fetch_str(y, vb, 2);
            if (strcmp(x, y) != 0) return 0;
        } else if (ta == TYPE_LIST && tb == TYPE_LIST) {
            if (!list_eq(va, vb)) return 0;
        } else if (ta == TYPE_TUPLE && tb == TYPE_TUPLE) {
            if (!list_eq(va, vb)) return 0;
        } else if (ta == TYPE_NONE && tb == TYPE_NONE) {
            /* equal */
        } else {
            return 0;
        }
    }
    return 1;
}

/* Deep value equality across all types */
uint8_t val_eq(uint8_t ta, long va, uint8_t tb, long vb) BANKED {
    if (IS_NUM(ta) && IS_NUM(tb)) return num_eq(ta, va, tb, vb);
    if (ta == TYPE_STR && tb == TYPE_STR) {
        char x[STR_MAX + 1], y[STR_MAX + 1];
        fetch_str(x, va, 2);
        fetch_str(y, vb, 2);
        return strcmp(x, y) == 0;
    }
    if (ta == TYPE_LIST && tb == TYPE_LIST) return list_eq(va, vb);
    if (ta == TYPE_TUPLE && tb == TYPE_TUPLE) return list_eq(va, vb);
    if (ta == TYPE_DICT && tb == TYPE_DICT) return dict_eq(va, vb);
    if (ta == TYPE_SET && tb == TYPE_SET) return dict_eq(va, vb);
    if (ta == TYPE_NONE && tb == TYPE_NONE) return 1;
    return 0;
}

/* Python dict equality: same keys, equal values (order-insensitive) */
uint8_t dict_eq(int a, int b) BANKED {
    int la = dict_len(a);
    int i, j;
    uint8_t kt, vt, kt2, vt2;
    long kv, vv, kv2, vv2;
    if (la != dict_len(b)) return 0;
    for (i = 0; i < la; i++) {
        dict_entry(a, i, &kt, &kv, &vt, &vv);
        j = dict_find(b, kt, kv, 2);
        if (j < 0) return 0;
        dict_entry(b, j, &kt2, &kv2, &vt2, &vv2);
        if (!val_eq(vt, vv, vt2, vv2)) return 0;
    }
    return 1;
}

/* Membership: needle (lv,lt) in list ptr */
uint8_t list_contains(int ptr, long lv, uint8_t lt, uint8_t lbank) BANKED {
    int len = list_len(ptr);
    int i;
    for (i = 0; i < len; i++) {
        uint8_t et;
        long ev = list_get(ptr, i, &et);
        if (IS_NUM(lt) && IS_NUM(et)) {
            if (num_eq(lt, lv, et, ev)) return 1;
        } else if (lt == TYPE_STR && et == TYPE_STR) {
            char x[STR_MAX + 1], y[STR_MAX + 1];
            fetch_str(x, lv, lbank);
            fetch_str(y, ev, 2);
            if (strcmp(x, y) == 0) return 1;
        } else if ((lt == TYPE_LIST && et == TYPE_LIST) ||
                   (lt == TYPE_TUPLE && et == TYPE_TUPLE)) {
            if (list_eq(lv, ev)) return 1;
        }
    }
    return 0;
}


/* --- Output window -------------------------------------------------------
   Fixed lines under "--- Out ---". Keeping output inside this window
   (instead of raw printf) stops long output from scrolling the gbdk
   console and wrecking the screen layout. */

#define OUT_ROWS 5
char out_lines[OUT_ROWS][21];
uint8_t out_count = 0;

void out_redraw(void) BANKED {
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

void out_putline(const char* s) BANKED {
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

void render_val(uint8_t t, long v, char* buf, uint8_t* pos);

void render_seq_inner(int ptr, char* buf, uint8_t* pos, uint8_t tuple) BANKED {
    int len = list_len(ptr);
    int i;
    buf[(*pos)++] = tuple ? '(' : '[';
    for (i = 0; i < len; i++) {
        uint8_t t;
        long v = list_get(ptr, i, &t);
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
    if (tuple && len == 1) {
        buf[(*pos)++] = ','; /* (1,) */
    }
    buf[(*pos)++] = tuple ? ')' : ']';
}

void render_list_inner(int ptr, char* buf, uint8_t* pos) {
    render_seq_inner(ptr, buf, pos, 0);
}

void render_dict_inner(int d, char* buf, uint8_t* pos, uint8_t is_set) BANKED {
    int len = dict_len(d);
    int i;
    buf[(*pos)++] = '{';
    for (i = 0; i < len; i++) {
        uint8_t kt, vt;
        long kv, vv;
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
        if (!is_set) {
            buf[(*pos)++] = ':';
            buf[(*pos)++] = ' ';
            render_val(vt, vv, buf, pos);
        }
    }
    buf[(*pos)++] = '}';
}

void render_val(uint8_t t, long v, char* buf, uint8_t* pos) {
    char tmp[20];
    uint8_t nl;
    if (t == TYPE_LIST || t == TYPE_DICT || t == TYPE_TUPLE || t == TYPE_SET) {
        if (*pos > 14) {
            buf[(*pos)++] = '.';
            buf[(*pos)++] = '.';
            return;
        }
        if (t == TYPE_LIST || t == TYPE_TUPLE) {
            render_seq_inner(v, buf, pos, t == TYPE_TUPLE);
        } else {
            render_dict_inner(v, buf, pos, t == TYPE_SET);
        }
        return;
    }
    if (t == TYPE_STR) {
        uint8_t i = 0;
        char* s = (char*)(uint16_t)v;
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
    } else if (t == TYPE_FLOAT) {
        render_float(v, tmp);
    } else {
        ltoa(v, tmp, 10);
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

void render_list(int ptr, char* buf) BANKED {
    uint8_t pos = 0;
    render_list_inner(ptr, buf, &pos);
    buf[pos] = '\0';
}

/* Copy a string value out of banked SRAM, then emit it on the output
   window. echo=1 gives the REPL "> 'x'" form, echo=0 the print() form. */
void emit_value(long val, uint8_t vtype, uint8_t str_bank, uint8_t echo) BANKED {
    if (vtype == TYPE_STR) {
        char temp[STR_MAX + 1];
        SWITCH_RAM(str_bank);
        strcpy(temp, (char*)(uint16_t)val);
        SWITCH_RAM(1);
        if (echo) {
            sprintf(line_buf, "> '%s'", temp);
        } else {
            sprintf(line_buf, "%s", temp);
        }
    } else if (vtype == TYPE_LIST || vtype == TYPE_DICT ||
               vtype == TYPE_TUPLE || vtype == TYPE_SET) {
        char temp[24];
        uint8_t pos = 0;
        if (vtype == TYPE_LIST || vtype == TYPE_TUPLE) {
            render_seq_inner(val, temp, &pos, vtype == TYPE_TUPLE);
        } else {
            render_dict_inner(val, temp, &pos, vtype == TYPE_SET);
        }
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
    } else if (vtype == TYPE_FLOAT) {
        char temp[24];
        render_float(val, temp);
        sprintf(line_buf, echo ? "> %s" : "%s", temp);
    } else {
        if (echo) {
            strcpy(line_buf, "> ");
            ltoa(val, line_buf + 2, 10);
        } else {
            ltoa(val, line_buf, 10);
        }
    }
    out_putline(line_buf);
}

