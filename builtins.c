/* gbpython builtins, in ROM bank 3. Arguments arrive pre-evaluated from
   the bank-1 evaluator; results go back via *result with the value channel
   (last_eval_type / last_eval_str_bank) set as usual. Returns 1 when the
   name matched a builtin (even if it raised), 0 to fall through to user
   functions. */

#pragma bank 3

#include <gb/gb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gbpython.h"

#define IS_NUM_ORD(t) ((t) == TYPE_INT || (t) == TYPE_BOOL || (t) == TYPE_FLOAT)

uint8_t call_builtin(const char* name, long* argv, uint8_t* arg_type,
                     uint8_t* arg_bank, uint8_t argc, long* result) BANKED {
    *result = 0;

    if (strcmp(name, "len") == 0) {
        last_eval_type = TYPE_INT;
        if (argc && arg_type[0] == TYPE_STR) {
            fetch_str(sbuf_l, argv[0], arg_bank[0]);
            *result = strlen(sbuf_l);
        } else if (argc && (arg_type[0] == TYPE_LIST || arg_type[0] == TYPE_TUPLE)) {
            *result = list_len(argv[0]);
        } else if (argc && (arg_type[0] == TYPE_DICT || arg_type[0] == TYPE_SET)) {
            *result = dict_len(argv[0]);
        } else {
            raise_error("TypeError: len");
        }
        return 1;
    }

    if (strcmp(name, "abs") == 0) {
        last_eval_type = TYPE_INT;
        if (argc && arg_type[0] == TYPE_FLOAT) {
            last_eval_type = TYPE_FLOAT;
            *result = f32_cmp(argv[0], 0) < 0 ? f32_neg(argv[0]) : argv[0];
        } else if (argc && (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL)) {
            *result = argv[0] < 0 ? -argv[0] : argv[0];
        } else {
            raise_error("TypeError: abs");
        }
        return 1;
    }

    if (strcmp(name, "str") == 0) {
        char tmp[24];
        char* dst;
        if (argc == 0) {
            tmp[0] = '\0';
        } else if (arg_type[0] == TYPE_STR) {
            last_eval_type = TYPE_STR;
            last_eval_str_bank = arg_bank[0];
            *result = argv[0];
            return 1;
        } else if (arg_type[0] == TYPE_BOOL) {
            strcpy(tmp, argv[0] ? "True" : "False");
        } else if (arg_type[0] == TYPE_NONE) {
            strcpy(tmp, "None");
        } else if (arg_type[0] == TYPE_LIST || arg_type[0] == TYPE_TUPLE ||
                   arg_type[0] == TYPE_DICT || arg_type[0] == TYPE_SET) {
            uint8_t rp = 0;
            if (arg_type[0] == TYPE_LIST || arg_type[0] == TYPE_TUPLE) {
                render_seq_inner(argv[0], tmp, &rp, arg_type[0] == TYPE_TUPLE);
            } else {
                render_dict_inner(argv[0], tmp, &rp, arg_type[0] == TYPE_SET);
            }
            tmp[rp] = '\0';
        } else if (arg_type[0] == TYPE_FLOAT) {
            render_float(argv[0], tmp);
        } else {
            ltoa(argv[0], tmp, 10);
        }
        SWITCH_RAM(2);
        dst = (char*)sram_str_alloc(strlen(tmp) + 1);
        if (dst != NULL) strcpy(dst, tmp);
        SWITCH_RAM(1);
        last_eval_type = TYPE_STR;
        last_eval_str_bank = 2;
        *result = dst != NULL ? (long)(uint16_t)dst : 0;
        return 1;
    }

    if (strcmp(name, "int") == 0) {
        last_eval_type = TYPE_INT;
        if (argc == 0) return 1;
        if (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL) {
            *result = argv[0];
            return 1;
        }
        if (arg_type[0] == TYPE_FLOAT) {
            *result = f32_trunc(argv[0]); /* truncates, like python */
            return 1;
        }
        if (arg_type[0] == TYPE_STR) {
            long v = 0;
            uint8_t j = 0;
            uint8_t neg = 0;
            uint8_t any = 0;
            fetch_str(sbuf_l, argv[0], arg_bank[0]);
            while (sbuf_l[j] == ' ') j++;
            if (sbuf_l[j] == '-') { neg = 1; j++; }
            else if (sbuf_l[j] == '+') j++;
            while (sbuf_l[j] >= '0' && sbuf_l[j] <= '9') {
                v = v * 10 + (sbuf_l[j] - '0');
                j++;
                any = 1;
            }
            while (sbuf_l[j] == ' ') j++;
            if (!any || sbuf_l[j] != '\0') {
                raise_error("ValueError: int");
                return 1;
            }
            *result = neg ? -v : v;
            return 1;
        }
        raise_error("TypeError: int");
        return 1;
    }

    if (strcmp(name, "float") == 0) {
        last_eval_type = TYPE_FLOAT;
        if (argc == 0) {
            *result = 0; /* 0.0 */
            return 1;
        }
        if (arg_type[0] == TYPE_FLOAT) {
            *result = argv[0];
            return 1;
        }
        if (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL) {
            *result = f32_from_int(argv[0]);
            return 1;
        }
        if (arg_type[0] == TYPE_STR) {
            long f = 0;
            long ten = f32_from_int(10);
            long scale;
            uint8_t j = 0;
            uint8_t neg = 0;
            uint8_t any = 0;
            scale = f32_div(f32_from_int(1), ten);
            fetch_str(sbuf_l, argv[0], arg_bank[0]);
            while (sbuf_l[j] == ' ') j++;
            if (sbuf_l[j] == '-') { neg = 1; j++; }
            else if (sbuf_l[j] == '+') j++;
            while (sbuf_l[j] >= '0' && sbuf_l[j] <= '9') {
                f = f32_add(f32_mul(f, ten), f32_from_int(sbuf_l[j] - '0'));
                j++;
                any = 1;
            }
            if (sbuf_l[j] == '.') {
                j++;
                while (sbuf_l[j] >= '0' && sbuf_l[j] <= '9') {
                    f = f32_add(f, f32_mul(f32_from_int(sbuf_l[j] - '0'), scale));
                    scale = f32_div(scale, ten);
                    j++;
                    any = 1;
                }
            }
            while (sbuf_l[j] == ' ') j++;
            if (!any || sbuf_l[j] != '\0') {
                last_eval_type = TYPE_INT;
                raise_error("ValueError: float");
                return 1;
            }
            *result = neg ? f32_neg(f) : f;
            return 1;
        }
        last_eval_type = TYPE_INT;
        raise_error("TypeError: float");
        return 1;
    }

    if (strcmp(name, "round") == 0) {
        last_eval_type = TYPE_INT;
        if (argc && arg_type[0] == TYPE_FLOAT) {
            static const long HALF = 0x3F000000L;
            if (f32_cmp(argv[0], 0) < 0) {
                *result = f32_trunc(f32_sub(argv[0], HALF));
            } else {
                *result = f32_trunc(f32_add(argv[0], HALF));
            }
        } else if (argc && (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL)) {
            *result = argv[0];
        } else {
            raise_error("TypeError: round");
        }
        return 1;
    }

    if (strcmp(name, "chr") == 0) {
        char* dst;
        last_eval_type = TYPE_INT;
        if (!argc || (arg_type[0] != TYPE_INT && arg_type[0] != TYPE_BOOL)) {
            raise_error("TypeError: chr");
            return 1;
        }
        SWITCH_RAM(2);
        dst = (char*)sram_str_alloc(2);
        if (dst != NULL) {
            dst[0] = (char)argv[0];
            dst[1] = '\0';
        }
        SWITCH_RAM(1);
        last_eval_type = TYPE_STR;
        last_eval_str_bank = 2;
        *result = dst != NULL ? (long)(uint16_t)dst : 0;
        return 1;
    }

    if (strcmp(name, "ord") == 0) {
        last_eval_type = TYPE_INT;
        if (argc && arg_type[0] == TYPE_STR) {
            fetch_str(sbuf_l, argv[0], arg_bank[0]);
            if (strlen(sbuf_l) == 1) {
                *result = (uint8_t)sbuf_l[0];
                return 1;
            }
        }
        raise_error("TypeError: ord");
        return 1;
    }

    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
        strcmp(name, "sum") == 0) {
        uint8_t is_min = (name[1] == 'i');
        uint8_t is_sum = (name[0] == 's');
        long best = 0;
        long total = 0;      /* exact while everything is int */
        long ftotal = 0;     /* float accumulator (bits) */
        uint8_t best_t = TYPE_INT;
        uint8_t got = 0;
        uint8_t any_float = 0;
        uint8_t et;
        long ev;
        int j, count;
        last_eval_type = TYPE_INT;
        count = (argc == 1 && (arg_type[0] == TYPE_LIST || arg_type[0] == TYPE_TUPLE))
                    ? list_len(argv[0]) : argc;
        for (j = 0; j < count; j++) {
            if (argc == 1 && (arg_type[0] == TYPE_LIST || arg_type[0] == TYPE_TUPLE)) {
                ev = list_get(argv[0], j, &et);
            } else {
                ev = argv[j];
                et = arg_type[j];
            }
            if (et != TYPE_INT && et != TYPE_BOOL && et != TYPE_FLOAT) {
                raise_error("TypeError");
                return 1;
            }
            if (et == TYPE_FLOAT && !any_float) {
                any_float = 1;
                ftotal = f32_from_int(total);
            }
            if (any_float) {
                ftotal = f32_add(ftotal, num_to_f32(ev, et));
            } else {
                total += ev;
            }
            if (!got ||
                (is_min ? f32_cmp(num_to_f32(ev, et), num_to_f32(best, best_t)) < 0
                        : f32_cmp(num_to_f32(ev, et), num_to_f32(best, best_t)) > 0)) {
                best = ev;
                best_t = et;
            }
            got = 1;
        }
        if (is_sum) {
            if (any_float) {
                last_eval_type = TYPE_FLOAT;
                *result = ftotal;
            } else {
                *result = total;
            }
            return 1;
        }
        if (!got) {
            raise_error("ValueError: empty");
            return 1;
        }
        last_eval_type = (best_t == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
        *result = (best_t == TYPE_BOOL) ? (best != 0) : best;
        return 1;
    }

    if (strcmp(name, "sorted") == 0) {
        int src, dst, n2, i2, j2;
        uint8_t et, et2;
        long ev, ev2;
        if (!argc || (arg_type[0] != TYPE_LIST && arg_type[0] != TYPE_TUPLE)) {
            raise_error("TypeError: sorted");
            return 1;
        }
        src = (int)argv[0];
        n2 = list_len(src);
        dst = list_new(n2);
        if (dst) {
            for (i2 = 0; i2 < n2; i2++) {
                ev = list_get(src, i2, &et);
                /* insertion sort */
                for (j2 = i2; j2 > 0; j2--) {
                    ev2 = list_get(dst, j2 - 1, &et2);
                    if (IS_NUM_ORD(et) && IS_NUM_ORD(et2)) {
                        if (f32_cmp(num_to_f32(ev2, et2), num_to_f32(ev, et)) <= 0) break;
                    } else if (et == TYPE_STR && et2 == TYPE_STR) {
                        fetch_str(sbuf_l, ev2, 2);
                        fetch_str(sbuf_r + 20, ev, 2); /* name is in sbuf_r[0..] */
                        if (strcmp(sbuf_l, sbuf_r + 20) <= 0) break;
                    } else {
                        raise_error("TypeError: sorted");
                        return 1;
                    }
                    list_set(dst, j2, ev2, et2);
                }
                list_set(dst, j2, ev, et);
            }
        }
        last_eval_type = TYPE_LIST;
        *result = dst;
        return 1;
    }

    if (strcmp(name, "input") == 0) {
        char tmp[17];
        char* dst;
        if (argc && arg_type[0] == TYPE_STR) {
            fetch_str(sbuf_l, argv[0], arg_bank[0]);
            out_putline(sbuf_l);
        }
        out_putline("? ");
        ui_input_line(tmp, 16);
        SWITCH_RAM(2);
        dst = (char*)sram_str_alloc(strlen(tmp) + 1);
        if (dst != NULL) strcpy(dst, tmp);
        SWITCH_RAM(1);
        last_eval_type = TYPE_STR;
        last_eval_str_bank = 2;
        *result = dst != NULL ? (long)(uint16_t)dst : 0;
        return 1;
    }

    return 0; /* not a builtin */
}
