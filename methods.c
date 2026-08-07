/* gbpython value methods ('x'.upper(), d.get(k), l.index(v), ...) in ROM
   bank 4. The method name is staged in sbuf_r (WRAM) by the caller since
   banked code can't read another bank's constants. */

#pragma bank 4

#include <gb/gb.h>
#include <stdio.h>
#include <string.h>
#include "gbpython.h"

/* --- Methods on built-in types -------------------------------------------
   The method name is staged in sbuf_r (WRAM) by the caller; string bases
   are staged in sbuf_l. */

static long new_str(const char* src) {
    char* dst;
    SWITCH_RAM(2);
    dst = (char*)sram_str_alloc(strlen(src) + 1);
    if (dst != NULL) strcpy(dst, src);
    SWITCH_RAM(1);
    last_eval_type = TYPE_STR;
    last_eval_str_bank = 2;
    return dst != NULL ? (long)(uint16_t)dst : 0;
}

uint8_t call_value_method(long base, uint8_t btype, uint8_t bbank,
                          long* argv, uint8_t* arg_type, uint8_t* arg_bank,
                          uint8_t argc, long* result) BANKED {
    const char* name = sbuf_r;
    *result = 0;

    if (btype == TYPE_STR) {
        char work[STR_MAX + 1];
        uint8_t i;
        fetch_str(sbuf_l, base, bbank);

        if (strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0) {
            uint8_t up = (name[0] == 'u');
            for (i = 0; sbuf_l[i]; i++) {
                char c = sbuf_l[i];
                if (up && c >= 'a' && c <= 'z') c -= 32;
                if (!up && c >= 'A' && c <= 'Z') c += 32;
                work[i] = c;
            }
            work[i] = '\0';
            *result = new_str(work);
            return 1;
        }
        if (strcmp(name, "strip") == 0) {
            uint8_t s2 = 0;
            uint8_t e = strlen(sbuf_l);
            while (sbuf_l[s2] == ' ') s2++;
            while (e > s2 && sbuf_l[e - 1] == ' ') e--;
            for (i = 0; s2 < e; i++, s2++) work[i] = sbuf_l[s2];
            work[i] = '\0';
            *result = new_str(work);
            return 1;
        }
        if (strcmp(name, "find") == 0) {
            uint8_t hl, nl2, j;
            last_eval_type = TYPE_INT;
            *result = -1;
            if (!argc || arg_type[0] != TYPE_STR) {
                raise_error("TypeError: find");
                return 1;
            }
            fetch_str(work, argv[0], arg_bank[0]);
            hl = strlen(sbuf_l);
            nl2 = strlen(work);
            if (nl2 > hl) return 1;
            for (i = 0; i <= hl - nl2; i++) {
                for (j = 0; j < nl2; j++) {
                    if (sbuf_l[i + j] != work[j]) break;
                }
                if (j == nl2) {
                    *result = i;
                    return 1;
                }
            }
            return 1;
        }
        if (strcmp(name, "split") == 0) {
            /* split on a separator (default: single space) */
            char sep = ' ';
            int lst;
            uint8_t start = 0;
            uint8_t count = 0;
            long parts[16];
            if (argc && arg_type[0] == TYPE_STR) {
                fetch_str(work, argv[0], arg_bank[0]);
                sep = work[0];
            }
            for (i = 0; ; i++) {
                if (sbuf_l[i] == sep || sbuf_l[i] == '\0') {
                    uint8_t k2, w = 0;
                    char piece[STR_MAX + 1];
                    for (k2 = start; k2 < i; k2++) piece[w++] = sbuf_l[k2];
                    piece[w] = '\0';
                    if (count < 16) parts[count++] = new_str(piece);
                    start = i + 1;
                    if (sbuf_l[i] == '\0') break;
                }
            }
            lst = list_new(count);
            if (lst) {
                for (i = 0; i < count; i++) {
                    list_set(lst, i, parts[i], TYPE_STR);
                }
            }
            last_eval_type = TYPE_LIST;
            *result = lst;
            return 1;
        }
        if (strcmp(name, "join") == 0) {
            /* sep.join(list_of_strings); sep is in sbuf_l */
            char out[STR_MAX + 1];
            uint8_t w = 0;
            int n2, i2;
            uint8_t et;
            long ev;
            char piece[STR_MAX + 1];
            if (!argc || (arg_type[0] != TYPE_LIST && arg_type[0] != TYPE_TUPLE)) {
                raise_error("TypeError: join");
                return 1;
            }
            n2 = list_len((int)argv[0]);
            for (i2 = 0; i2 < n2; i2++) {
                uint8_t k2;
                ev = list_get((int)argv[0], i2, &et);
                if (et != TYPE_STR) {
                    raise_error("TypeError: join");
                    return 1;
                }
                if (i2) {
                    for (k2 = 0; sbuf_l[k2] && w < STR_MAX; k2++) out[w++] = sbuf_l[k2];
                }
                fetch_str(piece, ev, 2);
                for (k2 = 0; piece[k2] && w < STR_MAX; k2++) out[w++] = piece[k2];
            }
            out[w] = '\0';
            *result = new_str(out);
            return 1;
        }
        if (strcmp(name, "replace") == 0) {
            char from[STR_MAX + 1];
            char to[40];
            uint8_t w = 0, fl2;
            if (argc < 2 || arg_type[0] != TYPE_STR || arg_type[1] != TYPE_STR) {
                raise_error("TypeError: replace");
                return 1;
            }
            fetch_str(from, argv[0], arg_bank[0]);
            fetch_str(to, argv[1], arg_bank[1]);
            fl2 = strlen(from);
            i = 0;
            while (sbuf_l[i] && w < STR_MAX) {
                if (fl2 && strncmp(sbuf_l + i, from, fl2) == 0) {
                    uint8_t k2;
                    for (k2 = 0; to[k2] && w < STR_MAX; k2++) work[w++] = to[k2];
                    i += fl2;
                } else {
                    work[w++] = sbuf_l[i++];
                }
            }
            work[w] = '\0';
            *result = new_str(work);
            return 1;
        }
        return 0;
    }

    if (btype == TYPE_DICT || btype == TYPE_SET) {
        if (strcmp(name, "get") == 0 && btype == TYPE_DICT) {
            int slot;
            uint8_t kt2, vt2;
            long kv2, vv2;
            if (!argc) {
                raise_error("TypeError: get");
                return 1;
            }
            slot = dict_find((int)base, arg_type[0], argv[0], arg_bank[0]);
            if (slot < 0) {
                if (argc > 1) {
                    last_eval_type = arg_type[1];
                    last_eval_str_bank = arg_bank[1];
                    *result = argv[1];
                } else {
                    last_eval_type = TYPE_NONE;
                    *result = 0;
                }
                return 1;
            }
            dict_entry((int)base, slot, &kt2, &kv2, &vt2, &vv2);
            last_eval_type = vt2;
            last_eval_str_bank = 2;
            *result = vv2;
            return 1;
        }
        if (strcmp(name, "keys") == 0 || strcmp(name, "values") == 0) {
            uint8_t want_keys = (name[0] == 'k');
            int n2 = dict_len((int)base);
            int lst = list_new(n2);
            int i2;
            uint8_t kt2, vt2;
            long kv2, vv2;
            if (lst) {
                for (i2 = 0; i2 < n2; i2++) {
                    dict_entry((int)base, i2, &kt2, &kv2, &vt2, &vv2);
                    if (want_keys) list_set(lst, i2, kv2, kt2);
                    else list_set(lst, i2, vv2, vt2);
                }
            }
            last_eval_type = TYPE_LIST;
            *result = lst;
            return 1;
        }
        return 0;
    }

    if (btype == TYPE_LIST) {
        if (strcmp(name, "append") == 0) {
            if (!argc) {
                raise_error("TypeError: append");
                return 1;
            }
            if (arg_type[0] == TYPE_STR) {
                argv[0] = store_str_value(argv[0], arg_bank[0]);
            }
            list_append((int)base, argv[0], arg_type[0]);
            last_eval_type = TYPE_NONE;
            return 1;
        }
        if (strcmp(name, "pop") == 0) {
            int n2 = list_len((int)base);
            int idx = n2 - 1;
            uint8_t et;
            if (argc) idx = (int)argv[0];
            if (idx < 0) idx += n2;
            if (n2 == 0 || idx < 0 || idx >= n2) {
                raise_error("IndexError: pop");
                return 1;
            }
            *result = list_get((int)base, idx, &et);
            list_remove_at((int)base, idx);
            last_eval_type = et;
            last_eval_str_bank = 2;
            return 1;
        }
    }

    if (btype == TYPE_LIST || btype == TYPE_TUPLE) {
        if (strcmp(name, "index") == 0) {
            int n2 = list_len((int)base);
            int i2;
            uint8_t et;
            long ev;
            last_eval_type = TYPE_INT;
            if (!argc) {
                raise_error("TypeError: index");
                return 1;
            }
            for (i2 = 0; i2 < n2; i2++) {
                ev = list_get((int)base, i2, &et);
                if (val_eq(arg_type[0], argv[0], et, ev)) {
                    *result = i2;
                    return 1;
                }
            }
            raise_error("ValueError: index");
            return 1;
        }
        if (strcmp(name, "count") == 0) {
            int n2 = list_len((int)base);
            int i2;
            long cnt = 0;
            uint8_t et;
            long ev;
            last_eval_type = TYPE_INT;
            for (i2 = 0; i2 < n2; i2++) {
                ev = list_get((int)base, i2, &et);
                if (argc && val_eq(arg_type[0], argv[0], et, ev)) cnt++;
            }
            *result = cnt;
            return 1;
        }
        return 0;
    }

    return 0;
}
