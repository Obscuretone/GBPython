#pragma bank 1

#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gbpython.h"

/* Environment Definitions */
typedef struct EnvNode {
    char identifier[16];
    int value;
    uint8_t vtype;
    struct EnvNode* next;
} EnvNode;

EnvNode* env = NULL;
/* env head at function entry; NULL at top level. Assignments search only
   the current frame's locals (python: assignment creates a local), reads
   search the whole chain and fall through to globals. */
EnvNode* frame_base = NULL;

EnvNode* get_variable_node(const char* id) {
    EnvNode* current = env;
    while (current) {
        if (strcmp(current->identifier, id) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void set_variable(const char* id, int val, uint8_t vtype, uint8_t str_bank) {
    EnvNode* current = env;
    if (vtype == TYPE_STR) {
        val = store_str_value(val, str_bank);
    }
    while (current && current != frame_base) {
        if (strcmp(current->identifier, id) == 0) {
            current->value = val;
            current->vtype = vtype;
            return;
        }
        current = current->next;
    }
    current = (EnvNode*)malloc(sizeof(EnvNode));
    if (current != NULL) {
        strcpy(current->identifier, id);
        current->value = val;
        current->vtype = vtype;
        current->next = env;
        env = current;
    }
}

/* Function registry: name -> AST_DEF node. The AST arena is wiped after
   every run, so definitions live only within the run that made them and
   the registry is cleared at the start of each run. */
typedef struct FuncReg {
    char name[16];
    ASTNode* def;
    struct FuncReg* next;
} FuncReg;

FuncReg* funcs = NULL;

void funcs_clear(void) {
    while (funcs) {
        FuncReg* t = funcs;
        funcs = funcs->next;
        free(t);
    }
}

FuncReg* find_func(const char* name) {
    FuncReg* f = funcs;
    while (f) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
        f = f->next;
    }
    return NULL;
}

void register_func(const char* name, ASTNode* def) {
    FuncReg* f = find_func(name);
    if (f != NULL) {
        f->def = def;
        return;
    }
    f = (FuncReg*)malloc(sizeof(FuncReg));
    if (f != NULL) {
        strcpy(f->name, name);
        f->def = def;
        f->next = funcs;
        funcs = f;
    }
}

int evaluate(ASTNode* n);

/* Evaluate a condition expression down to a C boolean */
uint8_t eval_cond(ASTNode* n) {
    int v = evaluate(n);
    return truthy(v, last_eval_type, last_eval_str_bank);
}

/* Operand-class results for eval_binop */
#define OPS_NUM 0   /* both numeric (int/bool) */
#define OPS_STR 1   /* both strings, sbuf_l/sbuf_r filled */
#define OPS_LIST 2  /* both lists */
#define OPS_MIXED 3
#define OPS_DICT 4  /* both dicts */

uint8_t eval_binop(ASTNode* n, int* lp, int* rp) {
    uint8_t ltype, lbank, rtype;
    *lp = evaluate(n->left);
    ltype = last_eval_type;
    lbank = last_eval_str_bank;
    *rp = evaluate(n->right);
    rtype = last_eval_type;
    if (ltype == TYPE_STR && rtype == TYPE_STR) {
        fetch_str(sbuf_r, *rp, last_eval_str_bank);
        fetch_str(sbuf_l, *lp, lbank);
        return OPS_STR;
    }
    if (ltype == TYPE_LIST && rtype == TYPE_LIST) {
        return OPS_LIST;
    }
    if (ltype == TYPE_DICT && rtype == TYPE_DICT) {
        return OPS_DICT;
    }
    if ((ltype == TYPE_INT || ltype == TYPE_BOOL) &&
        (rtype == TYPE_INT || rtype == TYPE_BOOL)) {
        return OPS_NUM;
    }
    return OPS_MIXED;
}

int evaluate(ASTNode* n) {
    if (!n) return 0;

    switch (n->type) {
        case AST_NUMBER:
            last_eval_type = TYPE_INT;
            return n->number;
        case AST_BOOL:
            last_eval_type = TYPE_BOOL;
            return n->number;
        case AST_NONE:
            last_eval_type = TYPE_NONE;
            return 0;
        case AST_IDENTIFIER: {
            EnvNode* node = get_variable_node(n->identifier);
            if (node) {
                last_eval_type = node->vtype;
                last_eval_str_bank = 2;
                return node->value;
            }
            last_eval_type = TYPE_INT;
            raise_error_name("NameError", n->identifier);
            return 0;
        }
        case AST_STRING:
            last_eval_type = TYPE_STR;
            last_eval_str_bank = 1;
            return (int)(n->identifier);
        case AST_ADD: {
            int l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            if (ops == OPS_STR) {
                return str_concat();
            }
            if (ops == OPS_LIST) {
                int ll = list_len(l);
                int rl = list_len(r);
                int dst = list_new(ll + rl);
                int i;
                uint8_t t;
                int v;
                if (dst) {
                    for (i = 0; i < ll; i++) {
                        v = list_get(l, i, &t);
                        list_set(dst, i, v, t);
                    }
                    for (i = 0; i < rl; i++) {
                        v = list_get(r, i, &t);
                        list_set(dst, ll + i, v, t);
                    }
                }
                last_eval_type = TYPE_LIST;
                return dst;
            }
            last_eval_type = TYPE_INT;
            if (ops == OPS_MIXED) {
                raise_error("TypeError: +");
                return 0;
            }
            return l + r;
        }
        case AST_SUB:
        case AST_MUL:
        case AST_DIV:
        case AST_MOD: {
            int l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            last_eval_type = TYPE_INT;
            if (ops != OPS_NUM) {
                raise_error("TypeError");
                return 0;
            }
            if (n->type == AST_SUB) return l - r;
            if (n->type == AST_MUL) return l * r;
            if (r == 0) {
                raise_error("ZeroDivisionError");
                return 0;
            }
            if (n->type == AST_DIV) {
                /* python floor division */
                int q = l / r;
                if ((l % r != 0) && ((l < 0) != (r < 0))) q--;
                return q;
            }
            {
                /* python modulo: result has the sign of the divisor */
                int m = l % r;
                if (m != 0 && ((m < 0) != (r < 0))) m += r;
                return m;
            }
        }
        case AST_NEG: {
            int v = evaluate(n->left);
            if (last_eval_type != TYPE_INT && last_eval_type != TYPE_BOOL) {
                raise_error("TypeError: -");
            }
            last_eval_type = TYPE_INT;
            return -v;
        }
        case AST_EQEQ:
        case AST_NEQ: {
            int l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            uint8_t eq;
            last_eval_type = TYPE_BOOL;
            if (ops == OPS_STR) eq = (strcmp(sbuf_l, sbuf_r) == 0);
            else if (ops == OPS_LIST) eq = list_eq(l, r);
            else if (ops == OPS_DICT) eq = (l == r); /* identity only */
            else if (ops == OPS_MIXED) eq = 0; /* python: mixed == is False */
            else eq = (l == r);
            return n->type == AST_EQEQ ? eq : !eq;
        }
        case AST_LT:
        case AST_GT:
        case AST_LE:
        case AST_GE: {
            int l, r;
            int c;
            uint8_t ops = eval_binop(n, &l, &r);
            last_eval_type = TYPE_BOOL;
            if (ops == OPS_STR) {
                c = strcmp(sbuf_l, sbuf_r);
                l = c;
                r = 0;
            } else if (ops != OPS_NUM) {
                raise_error("TypeError: order");
                return 0;
            }
            if (n->type == AST_LT) return l < r;
            if (n->type == AST_GT) return l > r;
            if (n->type == AST_LE) return l <= r;
            return l >= r;
        }
        case AST_AND: {
            /* Python semantics: return the deciding operand's value */
            int l = evaluate(n->left);
            uint8_t lt = last_eval_type;
            uint8_t lb = last_eval_str_bank;
            if (!truthy(l, lt, lb)) {
                last_eval_type = lt;
                last_eval_str_bank = lb;
                return l;
            }
            return evaluate(n->right);
        }
        case AST_OR: {
            int l = evaluate(n->left);
            uint8_t lt = last_eval_type;
            uint8_t lb = last_eval_str_bank;
            if (truthy(l, lt, lb)) {
                last_eval_type = lt;
                last_eval_str_bank = lb;
                return l;
            }
            return evaluate(n->right);
        }
        case AST_NOT: {
            uint8_t v = eval_cond(n->left);
            last_eval_type = TYPE_BOOL;
            return !v;
        }
        case AST_ASSIGN: {
            int val = evaluate(n->right);
            uint8_t vt = last_eval_type;
            if (exec_signal == SIG_ERROR) return 0;
            set_variable(n->identifier, val, vt, last_eval_str_bank);
            last_eval_type = vt;
            return val;
        }
        case AST_WHILE: {
            while (eval_cond(n->left) && exec_signal != SIG_ERROR) {
                if (n->right) evaluate(n->right);
                if (exec_signal == SIG_CONTINUE) {
                    exec_signal = SIG_NONE;
                } else if (exec_signal == SIG_BREAK) {
                    exec_signal = SIG_NONE;
                    break;
                } else if (exec_signal >= SIG_RETURN) {
                    break; /* return/error propagates to enclosing scope */
                }
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_IF: {
            ASTNode* pair = n->right;
            if (eval_cond(n->left)) {
                if (pair && pair->left) evaluate(pair->left);
            } else {
                if (pair && pair->right) evaluate(pair->right);
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_FOR: {
            int start = 0, stop = 0, step = 1;
            int v;
            ASTNode* r1 = n->left;
            if (r1) {
                if (r1->right) {
                    ASTNode* r2 = r1->right;
                    start = evaluate(r1->left);
                    stop = evaluate(r2->left);
                    if (r2->right) step = evaluate(r2->right);
                } else {
                    stop = evaluate(r1->left);
                }
            }
            if (step == 0) step = 1;
            for (v = start; step > 0 ? v < stop : v > stop; v += step) {
                set_variable(n->identifier, v, 0, 0);
                if (n->right) evaluate(n->right);
                if (exec_signal == SIG_CONTINUE) {
                    exec_signal = SIG_NONE;
                    continue;
                }
                if (exec_signal == SIG_BREAK) {
                    exec_signal = SIG_NONE;
                    break;
                }
                if (exec_signal >= SIG_RETURN) {
                    break;
                }
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_PRINT: {
            if (n->left) {
                int val = evaluate(n->left);
                emit_value(val, last_eval_type, last_eval_str_bank, 0);
            } else {
                out_putline("");
            }
            last_eval_type = 0;
            return 0;
        }
        case AST_DEF:
            register_func(n->identifier, n);
            last_eval_type = TYPE_NONE;
            return 0;
        case AST_CALL: {
            int argv[4];
            uint8_t arg_type[4];
            uint8_t arg_bank[4];
            uint8_t argc = 0;
            uint8_t i;
            ASTNode* a = n->left;
            ASTNode* param;
            FuncReg* f;
            EnvNode* saved_head;
            EnvNode* saved_frame;
            int result = 0;

            while (a && argc < 4) {
                argv[argc] = evaluate(a->left);
                arg_type[argc] = last_eval_type;
                arg_bank[argc] = last_eval_str_bank;
                argc++;
                a = a->right;
            }

            if (exec_signal == SIG_ERROR) return 0;

            if (strcmp(n->identifier, "len") == 0) {
                last_eval_type = TYPE_INT;
                if (argc && arg_type[0] == TYPE_STR) {
                    fetch_str(sbuf_l, argv[0], arg_bank[0]);
                    return strlen(sbuf_l);
                }
                if (argc && arg_type[0] == TYPE_LIST) {
                    return list_len(argv[0]);
                }
                if (argc && arg_type[0] == TYPE_DICT) {
                    return dict_len(argv[0]);
                }
                raise_error("TypeError: len");
                return 0;
            }
            if (strcmp(n->identifier, "abs") == 0) {
                last_eval_type = TYPE_INT;
                if (argc && (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL)) {
                    return argv[0] < 0 ? -argv[0] : argv[0];
                }
                raise_error("TypeError: abs");
                return 0;
            }
            if (strcmp(n->identifier, "str") == 0) {
                char tmp[24];
                char* dst;
                if (argc == 0) {
                    tmp[0] = '\0';
                } else if (arg_type[0] == TYPE_STR) {
                    last_eval_type = TYPE_STR;
                    last_eval_str_bank = arg_bank[0];
                    return argv[0];
                } else if (arg_type[0] == TYPE_BOOL) {
                    strcpy(tmp, argv[0] ? "True" : "False");
                } else if (arg_type[0] == TYPE_NONE) {
                    strcpy(tmp, "None");
                } else if (arg_type[0] == TYPE_LIST) {
                    render_list(argv[0], tmp);
                } else {
                    sprintf(tmp, "%d", argv[0]);
                }
                SWITCH_RAM(2);
                dst = (char*)sram_str_alloc(strlen(tmp) + 1);
                if (dst != NULL) strcpy(dst, tmp);
                SWITCH_RAM(1);
                last_eval_type = TYPE_STR;
                last_eval_str_bank = 2;
                return dst != NULL ? (int)dst : 0;
            }
            if (strcmp(n->identifier, "int") == 0) {
                last_eval_type = TYPE_INT;
                if (argc == 0) return 0;
                if (arg_type[0] == TYPE_INT || arg_type[0] == TYPE_BOOL) {
                    return argv[0];
                }
                if (arg_type[0] == TYPE_STR) {
                    int v = 0;
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
                        return 0;
                    }
                    return neg ? -v : v;
                }
                raise_error("TypeError: int");
                return 0;
            }
            if (strcmp(n->identifier, "chr") == 0) {
                char* dst;
                last_eval_type = TYPE_INT;
                if (!argc || (arg_type[0] != TYPE_INT && arg_type[0] != TYPE_BOOL)) {
                    raise_error("TypeError: chr");
                    return 0;
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
                return dst != NULL ? (int)dst : 0;
            }
            if (strcmp(n->identifier, "ord") == 0) {
                last_eval_type = TYPE_INT;
                if (argc && arg_type[0] == TYPE_STR) {
                    fetch_str(sbuf_l, argv[0], arg_bank[0]);
                    if (strlen(sbuf_l) == 1) {
                        return (uint8_t)sbuf_l[0];
                    }
                }
                raise_error("TypeError: ord");
                return 0;
            }
            if (strcmp(n->identifier, "min") == 0 ||
                strcmp(n->identifier, "max") == 0 ||
                strcmp(n->identifier, "sum") == 0) {
                uint8_t is_min = (n->identifier[1] == 'i');
                uint8_t is_sum = (n->identifier[0] == 's');
                int best = 0;
                int total = 0;
                uint8_t got = 0;
                last_eval_type = TYPE_INT;
                if (argc == 1 && arg_type[0] == TYPE_LIST) {
                    int llen = list_len(argv[0]);
                    int j2;
                    uint8_t et;
                    int ev;
                    for (j2 = 0; j2 < llen; j2++) {
                        ev = list_get(argv[0], j2, &et);
                        if (et != TYPE_INT && et != TYPE_BOOL) {
                            raise_error("TypeError");
                            return 0;
                        }
                        total += ev;
                        if (!got || (is_min ? ev < best : ev > best)) best = ev;
                        got = 1;
                    }
                } else {
                    for (i = 0; i < argc; i++) {
                        if (arg_type[i] != TYPE_INT && arg_type[i] != TYPE_BOOL) {
                            raise_error("TypeError");
                            return 0;
                        }
                        total += argv[i];
                        if (!got || (is_min ? argv[i] < best : argv[i] > best)) {
                            best = argv[i];
                        }
                        got = 1;
                    }
                }
                if (is_sum) return total;
                if (!got) {
                    raise_error("ValueError: empty");
                    return 0;
                }
                return best;
            }
            if (strcmp(n->identifier, "input") == 0) {
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
                return dst != NULL ? (int)dst : 0;
            }

            f = find_func(n->identifier);
            if (f == NULL) {
                last_eval_type = TYPE_INT;
                raise_error_name("NameError", n->identifier);
                return 0;
            }
            saved_head = env;
            saved_frame = frame_base;
            frame_base = saved_head;
            param = f->def->left;
            i = 0;
            while (param && i < argc) {
                set_variable(param->identifier, argv[i], arg_type[i], arg_bank[i]);
                param = param->right;
                i++;
            }
            if (param != NULL || i < argc) {
                /* arity mismatch: too few or too many arguments */
                while (env != saved_head) {
                    EnvNode* t = env;
                    env = env->next;
                    free(t);
                }
                frame_base = saved_frame;
                raise_error_name("TypeError", n->identifier);
                return 0;
            }
            if (f->def->right) {
                evaluate(f->def->right);
            }
            if (exec_signal == SIG_RETURN) {
                exec_signal = SIG_NONE;
                result = return_value;
                last_eval_type = return_type;
                last_eval_str_bank = return_str_bank;
            } else if (exec_signal == SIG_ERROR) {
                last_eval_type = TYPE_INT;
            } else {
                exec_signal = SIG_NONE;
                last_eval_type = TYPE_NONE; /* no return: None */
            }
            /* pop this frame's locals */
            while (env != saved_head) {
                EnvNode* t = env;
                env = env->next;
                free(t);
            }
            frame_base = saved_frame;
            return result;
        }
        case AST_RETURN: {
            if (n->left) {
                return_value = evaluate(n->left);
                return_type = last_eval_type;
                return_str_bank = last_eval_str_bank;
                if (exec_signal == SIG_ERROR) return 0;
            } else {
                return_value = 0;
                return_type = TYPE_NONE;
            }
            exec_signal = SIG_RETURN;
            return return_value;
        }
        case AST_BREAK:
            exec_signal = SIG_BREAK;
            return 0;
        case AST_CONTINUE:
            exec_signal = SIG_CONTINUE;
            return 0;
        case AST_PASS:
        case AST_PARAM:
        case AST_ARG:
            last_eval_type = TYPE_NONE;
            return 0;
        case AST_INDEX: {
            int base, idx, len;
            uint8_t btype, bbank, ktype, kbank;
            base = evaluate(n->left);
            btype = last_eval_type;
            bbank = last_eval_str_bank;
            idx = evaluate(n->right);
            ktype = last_eval_type;
            kbank = last_eval_str_bank;
            if (exec_signal == SIG_ERROR) return 0;
            if (btype == TYPE_DICT) {
                int slot = dict_find(base, ktype, idx, kbank);
                uint8_t kt2, vt2;
                int kv2, vv2;
                if (slot < 0) {
                    last_eval_type = TYPE_INT;
                    raise_error("KeyError");
                    return 0;
                }
                dict_entry(base, slot, &kt2, &kv2, &vt2, &vv2);
                last_eval_type = vt2;
                last_eval_str_bank = 2;
                return vv2;
            }
            if (ktype != TYPE_INT && ktype != TYPE_BOOL) {
                raise_error("TypeError: index");
                return 0;
            }
            last_eval_type = TYPE_INT;
            if (btype == TYPE_STR) {
                char* dst;
                fetch_str(sbuf_l, base, bbank);
                len = strlen(sbuf_l);
                if (idx < 0) idx += len;
                if (idx < 0 || idx >= len) {
                    raise_error("IndexError");
                    return 0;
                }
                SWITCH_RAM(2);
                dst = (char*)sram_str_alloc(2);
                if (dst != NULL) {
                    dst[0] = sbuf_l[idx];
                    dst[1] = '\0';
                }
                SWITCH_RAM(1);
                last_eval_type = TYPE_STR;
                last_eval_str_bank = 2;
                return dst != NULL ? (int)dst : 0;
            }
            if (btype == TYPE_LIST) {
                uint8_t et;
                int ev;
                len = list_len(base);
                if (idx < 0) idx += len;
                if (idx < 0 || idx >= len) {
                    raise_error("IndexError");
                    return 0;
                }
                ev = list_get(base, idx, &et);
                last_eval_type = et;
                last_eval_str_bank = 2;
                return ev;
            }
            raise_error("TypeError: subscript");
            return 0;
        }
        case AST_DICT: {
            int d = dict_new();
            ASTNode* a = n->left;
            while (a && d) {
                int kv, vv;
                uint8_t kt, vt;
                ASTNode* val_arg = a->right;
                kv = evaluate(a->left);
                kt = last_eval_type;
                if (kt == TYPE_STR) {
                    kv = store_str_value(kv, last_eval_str_bank);
                } else if (kt != TYPE_INT && kt != TYPE_BOOL) {
                    raise_error("TypeError: key");
                    return 0;
                }
                vv = evaluate(val_arg ? val_arg->left : NULL);
                vt = last_eval_type;
                if (vt == TYPE_STR) {
                    vv = store_str_value(vv, last_eval_str_bank);
                }
                if (exec_signal == SIG_ERROR) return 0;
                dict_set(d, kt, kv, vt, vv);
                a = val_arg ? val_arg->right : NULL;
            }
            last_eval_type = TYPE_DICT;
            return d;
        }
        case AST_MULTI: {
            int vals[4];
            uint8_t vts[4];
            uint8_t vbs[4];
            uint8_t nv = 0;
            uint8_t nt = 0;
            uint8_t i;
            ASTNode* a = n->right;
            ASTNode* t = n->left;
            while (a && nv < 4) {
                vals[nv] = evaluate(a->left);
                vts[nv] = last_eval_type;
                vbs[nv] = last_eval_str_bank;
                nv++;
                a = a->right;
            }
            if (exec_signal == SIG_ERROR) return 0;
            while (t) {
                nt++;
                t = t->right;
            }
            if (nt != nv) {
                raise_error("ValueError: unpack");
                return 0;
            }
            t = n->left;
            for (i = 0; i < nv; i++) {
                set_variable(t->identifier, vals[i], vts[i], vbs[i]);
                t = t->right;
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_LIST: {
            int dst;
            int count = 0;
            int i = 0;
            int v;
            uint8_t t;
            ASTNode* a = n->left;
            while (a) {
                count++;
                a = a->right;
            }
            dst = list_new(count);
            a = n->left;
            while (a && dst) {
                v = evaluate(a->left);
                t = last_eval_type;
                if (exec_signal == SIG_ERROR) return 0;
                if (t == TYPE_STR) {
                    v = store_str_value(v, last_eval_str_bank);
                }
                list_set(dst, i, v, t);
                i++;
                a = a->right;
            }
            last_eval_type = TYPE_LIST;
            return dst;
        }
        case AST_STORE: {
            ASTNode* target = n->left; /* AST_INDEX node */
            int base, idx, val, len;
            uint8_t btype, vt, ktype, kbank;
            base = evaluate(target->left);
            btype = last_eval_type;
            idx = evaluate(target->right);
            ktype = last_eval_type;
            kbank = last_eval_str_bank;
            val = evaluate(n->right);
            vt = last_eval_type;
            if (exec_signal == SIG_ERROR) return 0;
            last_eval_type = TYPE_NONE;
            if (btype == TYPE_DICT) {
                if (ktype == TYPE_STR) {
                    idx = store_str_value(idx, kbank);
                } else if (ktype != TYPE_INT && ktype != TYPE_BOOL) {
                    raise_error("TypeError: key");
                    return 0;
                }
                if (vt == TYPE_STR) {
                    val = store_str_value(val, last_eval_str_bank);
                }
                dict_set(base, ktype, idx, vt, val);
                return val;
            }
            if (btype != TYPE_LIST) {
                raise_error("TypeError: item asgn");
                return 0;
            }
            len = list_len(base);
            if (idx < 0) idx += len;
            if (idx < 0 || idx >= len) {
                raise_error("IndexError");
                return 0;
            }
            if (vt == TYPE_STR) {
                val = store_str_value(val, last_eval_str_bank);
            }
            list_set(base, idx, val, vt);
            return val;
        }
        case AST_FORIN: {
            int seq_val = evaluate(n->left);
            uint8_t stype = last_eval_type;
            uint8_t sbank = last_eval_str_bank;
            int len, i;
            uint8_t et;
            int ev;
            if (stype == TYPE_DICT) {
                /* python: iterating a dict yields its keys */
                len = dict_len(seq_val);
                for (i = 0; i < len; i++) {
                    uint8_t kt, vt2;
                    int kv, vv2;
                    dict_entry(seq_val, i, &kt, &kv, &vt2, &vv2);
                    set_variable(n->identifier, kv, kt, 2);
                    if (n->right) evaluate(n->right);
                    if (exec_signal == SIG_CONTINUE) {
                        exec_signal = SIG_NONE;
                        continue;
                    }
                    if (exec_signal == SIG_BREAK) {
                        exec_signal = SIG_NONE;
                        break;
                    }
                    if (exec_signal >= SIG_RETURN) break;
                }
                last_eval_type = TYPE_NONE;
                return 0;
            }
            if (stype == TYPE_LIST) {
                len = list_len(seq_val);
                for (i = 0; i < len; i++) {
                    ev = list_get(seq_val, i, &et);
                    set_variable(n->identifier, ev, et, 2);
                    if (n->right) evaluate(n->right);
                    if (exec_signal == SIG_CONTINUE) {
                        exec_signal = SIG_NONE;
                        continue;
                    }
                    if (exec_signal == SIG_BREAK) {
                        exec_signal = SIG_NONE;
                        break;
                    }
                    if (exec_signal >= SIG_RETURN) break;
                }
            } else if (stype == TYPE_STR) {
                char iter[33];
                char ch[2];
                fetch_str(iter, seq_val, sbank);
                len = strlen(iter);
                ch[1] = '\0';
                for (i = 0; i < len; i++) {
                    ch[0] = iter[i];
                    /* bind as a bank-1-style literal so it's copied to
                       the persistent arena by set_variable */
                    set_variable(n->identifier, (int)ch, TYPE_STR, 1);
                    if (n->right) evaluate(n->right);
                    if (exec_signal == SIG_CONTINUE) {
                        exec_signal = SIG_NONE;
                        continue;
                    }
                    if (exec_signal == SIG_BREAK) {
                        exec_signal = SIG_NONE;
                        break;
                    }
                    if (exec_signal >= SIG_RETURN) break;
                }
            } else if (exec_signal != SIG_ERROR) {
                raise_error("TypeError: not iter");
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_IN: {
            int l, r;
            uint8_t lt, lb, rt;
            l = evaluate(n->left);
            lt = last_eval_type;
            lb = last_eval_str_bank;
            r = evaluate(n->right);
            rt = last_eval_type;
            if (exec_signal == SIG_ERROR) return 0;
            last_eval_type = TYPE_BOOL;
            if (rt == TYPE_LIST) {
                return list_contains(r, l, lt, lb);
            }
            if (rt == TYPE_DICT) {
                /* python: 'k' in d tests keys */
                return dict_find(r, lt, l, lb) >= 0;
            }
            if (rt == TYPE_STR && lt == TYPE_STR) {
                /* python substring test */
                uint8_t i2, j2;
                uint8_t hl, nl2;
                fetch_str(sbuf_l, r, last_eval_str_bank);
                fetch_str(sbuf_r, l, lb);
                hl = strlen(sbuf_l);
                nl2 = strlen(sbuf_r);
                if (nl2 == 0) return 1;
                if (nl2 > hl) return 0;
                for (i2 = 0; i2 <= hl - nl2; i2++) {
                    for (j2 = 0; j2 < nl2; j2++) {
                        if (sbuf_l[i2 + j2] != sbuf_r[j2]) break;
                    }
                    if (j2 == nl2) return 1;
                }
                return 0;
            }
            raise_error("TypeError: in");
            return 0;
        }
        case AST_SLICE: {
            int base, start, stop, len;
            uint8_t btype, bbank;
            uint8_t has_start, has_stop;
            ASTNode* bounds = n->right;
            base = evaluate(n->left);
            btype = last_eval_type;
            bbank = last_eval_str_bank;
            has_start = (bounds && bounds->left);
            has_stop = (bounds && bounds->right);
            /* evaluate bounds before fetching: they may clobber sbuf_l */
            start = has_start ? evaluate(bounds->left) : 0;
            stop = has_stop ? evaluate(bounds->right) : 0;
            if (exec_signal == SIG_ERROR) return 0;
            if (btype == TYPE_STR) {
                fetch_str(sbuf_l, base, bbank);
                len = strlen(sbuf_l);
            } else if (btype == TYPE_LIST) {
                len = list_len(base);
            } else {
                raise_error("TypeError: slice");
                return 0;
            }
            if (!has_stop) stop = len;
            if (start < 0) start += len;
            if (stop < 0) stop += len;
            if (start < 0) start = 0;
            if (stop > len) stop = len;
            if (start > stop) start = stop;
            if (btype == TYPE_STR) {
                char* dst;
                uint8_t i2;
                SWITCH_RAM(2);
                dst = (char*)sram_str_alloc(stop - start + 1);
                if (dst != NULL) {
                    for (i2 = 0; i2 < stop - start; i2++) {
                        dst[i2] = sbuf_l[start + i2];
                    }
                    dst[stop - start] = '\0';
                }
                SWITCH_RAM(1);
                last_eval_type = TYPE_STR;
                last_eval_str_bank = 2;
                return dst != NULL ? (int)dst : 0;
            }
            {
                int dst = list_new(stop - start);
                int i2;
                uint8_t et;
                int ev;
                if (dst) {
                    for (i2 = 0; i2 < stop - start; i2++) {
                        ev = list_get(base, start + i2, &et);
                        list_set(dst, i2, ev, et);
                    }
                }
                last_eval_type = TYPE_LIST;
                return dst;
            }
        }
        case AST_ELSE:
            return 0; /* handled inside AST_IF */
        case AST_SEQ: {
            int val = evaluate(n->left);
            if (exec_signal != SIG_NONE) {
                return val; /* break/continue/return aborts the sequence */
            }
            if (n->right) {
                return evaluate(n->right);
            }
            return val;
        }
    }
    return 0;
}

/* Top-level execution: python REPL semantics. Expression statements echo
   their value; assignments, compound statements, and print() do not. */
void exec_statement(ASTNode* n) {
    int res;
    if (!n || exec_signal == SIG_ERROR) return;
    switch (n->type) {
        case AST_SEQ:
            exec_statement(n->left);
            exec_statement(n->right);
            return;
        case AST_ASSIGN:
        case AST_WHILE:
        case AST_IF:
        case AST_FOR:
        case AST_FORIN:
        case AST_PRINT:
        case AST_DEF:
        case AST_PASS:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_RETURN:
        case AST_STORE:
        case AST_MULTI:
            evaluate(n);
            if (exec_signal != SIG_ERROR) {
                exec_signal = SIG_NONE; /* stray signals stop at top level */
            }
            return;
        default:
            res = evaluate(n);
            if (exec_signal == SIG_ERROR) return;
            exec_signal = SIG_NONE;
            if (last_eval_type != TYPE_NONE) {
                /* python REPL: None results print nothing */
                emit_value(res, last_eval_type, last_eval_str_bank, 1);
            }
            return;
    }
}

/* Wipe the entire interpreter state: variables, functions, all arenas.
   Used when an arena fills up (MemoryError). */
void full_reset(void) {
    while (env) {
        EnvNode* t = env;
        env = env->next;
        free(t);
    }
    funcs_clear();
    frame_base = NULL;
    sram_ast_reset();
    sram_def_ptr = (uint8_t*)0xC000;
    def_mode = 0;
    sram_str_ptr = (uint8_t*)0xA000;
    sram_list_ptr = (uint8_t*)0xA000;
}

void run_interpreter(void) BANKED {
    ASTNode* ast;

    out_count = 0;
    out_redraw();

    frame_base = NULL;
    exec_signal = SIG_NONE;

    /* Enable SRAM and switch to Bank 1 for parsing and evaluation */
    ENABLE_RAM;
    SWITCH_RAM(1);

    lexer_reset(input_buffer);
    next_token();

    ast = parse_program();
    if (exec_signal == SIG_ERROR) {
        /* arena filled during parse: wipe everything */
        out_putline(err_buf);
        out_putline("(state cleared)");
        exec_signal = SIG_NONE;
        full_reset();
    } else if (ast) {
        exec_statement(ast);
        if (exec_signal == SIG_ERROR) {
            out_putline(err_buf);
            exec_signal = SIG_NONE;
            if (strcmp(err_buf, "MemoryError") == 0) {
                out_putline("(state cleared)");
                full_reset();
            }
        }
    } else if (input_len > 0) {
        out_putline("SyntaxError");
    }
    /* Wipe the per-run arena; def subtrees live in the persistent arena
       at the top of the bank and survive (a real REPL keeps definitions) */
    sram_ast_reset();

    /* Protect SRAM from corruption during non-write states */
    DISABLE_RAM;

    /* Clear input buffer after command execution to start a fresh line */
    input_len = 0;
    input_buffer[0] = '\0';
    draw_input_buffer();
}

