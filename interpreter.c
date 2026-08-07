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
    long value;
    uint8_t vtype;
    struct EnvNode* next;
} EnvNode;

EnvNode* env = NULL;
/* env head at function entry; NULL at top level. Assignments search only
   the current frame's locals (python: assignment creates a local). Reads
   search the current frame, then jump straight to the globals — never
   other call frames' locals (python lexical scoping). */
EnvNode* frame_base = NULL;
EnvNode* globals_head = NULL; /* env head when the outermost call began */

EnvNode* get_variable_node(const char* id) {
    EnvNode* current = env;
    EnvNode* stop = frame_base;
    while (current != stop) {
        if (strcmp(current->identifier, id) == 0) {
            return current;
        }
        current = current->next;
    }
    if (frame_base != NULL) {
        current = globals_head;
        while (current) {
            if (strcmp(current->identifier, id) == 0) {
                return current;
            }
            current = current->next;
        }
    }
    return NULL;
}

void set_variable(const char* id, long val, uint8_t vtype, uint8_t str_bank) {
    EnvNode* current = env;
    if (vtype == TYPE_STR) {
        val = store_str_value(val, str_bank);
    }
    while (current != frame_base) {
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

/* Class registry: name -> method list. Method defs live in the persistent
   def arena, so classes survive across runs like functions do. An
   instance is a dict whose first entry has a TYPE_NONE key (impossible
   from user code) holding the ClassReg pointer. */
typedef struct MethodReg {
    char name[16];
    ASTNode* def;
    struct MethodReg* next;
} MethodReg;

typedef struct ClassReg {
    char name[16];
    MethodReg* methods;
    struct ClassReg* next;
} ClassReg;

ClassReg* classes = NULL;
ClassReg* current_class = NULL; /* set while evaluating a class body */

void classes_clear(void) {
    while (classes) {
        ClassReg* c = classes;
        MethodReg* m = c->methods;
        while (m) {
            MethodReg* t = m;
            m = m->next;
            free(t);
        }
        classes = c->next;
        free(c);
    }
}

ClassReg* find_class(const char* name) {
    ClassReg* c = classes;
    while (c) {
        if (strcmp(c->name, name) == 0) return c;
        c = c->next;
    }
    return NULL;
}

MethodReg* find_method(ClassReg* c, const char* name) {
    MethodReg* m = c->methods;
    while (m) {
        if (strcmp(m->name, name) == 0) return m;
        m = m->next;
    }
    return NULL;
}

long evaluate(ASTNode* n);

/* Evaluate a condition expression down to a C boolean */
uint8_t eval_cond(ASTNode* n) {
    long v = evaluate(n);
    return truthy(v, last_eval_type, last_eval_str_bank);
}

/* Operand-class results for eval_binop */
#define OPS_NUM 0   /* both numeric (int/bool/float) */
#define OPS_STR 1   /* both strings, sbuf_l/sbuf_r filled */
#define OPS_LIST 2  /* both lists */
#define OPS_MIXED 3
#define OPS_DICT 4  /* both dicts */

#define IS_NUM_T(t) ((t) == TYPE_INT || (t) == TYPE_BOOL || (t) == TYPE_FLOAT)

uint8_t binop_ltype, binop_rtype;
#define BINOP_FLOAT (binop_ltype == TYPE_FLOAT || binop_rtype == TYPE_FLOAT)

uint8_t eval_binop(ASTNode* n, long* lp, long* rp) {
    uint8_t lbank, ltype;
    *lp = evaluate(n->left);
    /* capture locally: evaluating the right side may run nested binops
       that clobber the globals */
    ltype = last_eval_type;
    lbank = last_eval_str_bank;
    *rp = evaluate(n->right);
    binop_ltype = ltype;
    binop_rtype = last_eval_type;
    if (binop_ltype == TYPE_STR && binop_rtype == TYPE_STR) {
        fetch_str(sbuf_r, *rp, last_eval_str_bank);
        fetch_str(sbuf_l, *lp, lbank);
        return OPS_STR;
    }
    if ((binop_ltype == TYPE_LIST && binop_rtype == TYPE_LIST) ||
        (binop_ltype == TYPE_TUPLE && binop_rtype == TYPE_TUPLE)) {
        return OPS_LIST;
    }
    if ((binop_ltype == TYPE_DICT && binop_rtype == TYPE_DICT) ||
        (binop_ltype == TYPE_SET && binop_rtype == TYPE_SET)) {
        return OPS_DICT;
    }
    if (IS_NUM_T(binop_ltype) && IS_NUM_T(binop_rtype)) {
        return OPS_NUM;
    }
    return OPS_MIXED;
}

/* One comparison step with python numeric/string semantics */
uint8_t compare_op(ASTNodeType op, uint8_t lt, long l, uint8_t lb,
                   uint8_t rt, long r, uint8_t rb) {
    int c;
    if (op == AST_IN) {
        /* handled by the caller (needs haystack semantics) */
        return 0;
    }
    if (lt == TYPE_STR && rt == TYPE_STR) {
        fetch_str(sbuf_l, l, lb);
        fetch_str(sbuf_r, r, rb);
        c = strcmp(sbuf_l, sbuf_r);
        l = c;
        r = 0;
        lt = rt = TYPE_INT;
    }
    if (op == AST_EQEQ || op == AST_NEQ) {
        uint8_t eq;
        if (lt == TYPE_INT && rt == TYPE_INT) eq = (l == r);
        else eq = val_eq(lt, l, rt, r);
        return op == AST_EQEQ ? eq : !eq;
    }
    if (!IS_NUM_T(lt) || !IS_NUM_T(rt)) {
        raise_error("TypeError: order");
        return 0;
    }
    if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
        int8_t c2 = f32_cmp(num_to_f32(l, lt), num_to_f32(r, rt));
        if (op == AST_LT) return c2 < 0;
        if (op == AST_GT) return c2 > 0;
        if (op == AST_LE) return c2 <= 0;
        return c2 >= 0;
    }
    if (op == AST_LT) return l < r;
    if (op == AST_GT) return l > r;
    if (op == AST_LE) return l <= r;
    return l >= r;
}

static long eval_list_like(ASTNode* n);
static long eval_call(ASTNode* n);
static long eval_attr(ASTNode* n);
static long eval_setattr(ASTNode* n);
static long eval_method(ASTNode* n);
static long invoke_function(ASTNode* def, const char* name, long* argv,
                            uint8_t* arg_type, uint8_t* arg_bank,
                            uint8_t argc);
static long eval_forin(ASTNode* n);
static long eval_slice(ASTNode* n);
static long eval_chain(ASTNode* n);
static long eval_dict_lit(ASTNode* n);
static long eval_multi(ASTNode* n);
static long eval_store(ASTNode* n);
static long eval_index(ASTNode* n);
static long eval_in(ASTNode* n);

long evaluate(ASTNode* n) {
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
        case AST_FLOAT:
            last_eval_type = TYPE_FLOAT;
            return n->number;
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
            return (long)(uint16_t)(n->identifier);
        case AST_ADD: {
            long l, r;
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
                long v;
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
                last_eval_type = binop_ltype; /* list+list or tuple+tuple */
                return dst;
            }
            if (ops == OPS_DICT) {
                last_eval_type = TYPE_INT;
                raise_error("TypeError: +");
                return 0;
            }
            if (ops == OPS_MIXED) {
                last_eval_type = TYPE_INT;
                raise_error("TypeError: +");
                return 0;
            }
            if (BINOP_FLOAT) {
                last_eval_type = TYPE_FLOAT;
                return f32_add(num_to_f32(l, binop_ltype),
                               num_to_f32(r, binop_rtype));
            }
            last_eval_type = TYPE_INT;
            return l + r;
        }
        case AST_SUB:
        case AST_MUL:
        case AST_DIV:
        case AST_TRUEDIV:
        case AST_MOD: {
            long l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            last_eval_type = TYPE_INT;
            if (ops != OPS_NUM) {
                raise_error("TypeError");
                return 0;
            }
            if (BINOP_FLOAT || n->type == AST_TRUEDIV) {
                /* float arithmetic; '/' is always true division (python 3) */
                long a = num_to_f32(l, binop_ltype);
                long b = num_to_f32(r, binop_rtype);
                long fq;
                last_eval_type = TYPE_FLOAT;
                if (n->type == AST_SUB) return f32_sub(a, b);
                if (n->type == AST_MUL) return f32_mul(a, b);
                if (f32_is_zero(b)) {
                    last_eval_type = TYPE_INT;
                    raise_error("ZeroDivisionError");
                    return 0;
                }
                if (n->type == AST_TRUEDIV) return f32_div(a, b);
                fq = f32_floor(f32_div(a, b));
                if (n->type == AST_DIV) {
                    if (BINOP_FLOAT) return f32_from_int(fq);
                    last_eval_type = TYPE_INT;
                    return fq;
                }
                /* python modulo: sign of the divisor */
                return f32_sub(a, f32_mul(f32_from_int(fq), b));
            }
            if (n->type == AST_SUB) return l - r;
            if (n->type == AST_MUL) return l * r;
            if (r == 0) {
                raise_error("ZeroDivisionError");
                return 0;
            }
            if (n->type == AST_DIV) {
                /* python floor division */
                long q = l / r;
                if ((l % r != 0) && ((l < 0) != (r < 0))) q--;
                return q;
            }
            {
                /* python modulo: result has the sign of the divisor */
                long m = l % r;
                if (m != 0 && ((m < 0) != (r < 0))) m += r;
                return m;
            }
        }
        case AST_NEG: {
            long v = evaluate(n->left);
            if (last_eval_type == TYPE_FLOAT) {
                return f32_neg(v);
            }
            if (last_eval_type != TYPE_INT && last_eval_type != TYPE_BOOL) {
                raise_error("TypeError: -");
            }
            last_eval_type = TYPE_INT;
            return -v;
        }
        case AST_EQEQ:
        case AST_NEQ: {
            long l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            uint8_t eq;
            last_eval_type = TYPE_BOOL;
            if (ops == OPS_STR) eq = (strcmp(sbuf_l, sbuf_r) == 0);
            else if (ops == OPS_LIST) eq = list_eq(l, r);
            else if (ops == OPS_DICT) eq = dict_eq(l, r);
            else if (ops == OPS_MIXED) eq = 0; /* python: mixed == is False */
            else eq = num_eq(binop_ltype, l, binop_rtype, r);
            return n->type == AST_EQEQ ? eq : !eq;
        }
        case AST_LT:
        case AST_GT:
        case AST_LE:
        case AST_GE: {
            long l, r;
            uint8_t ops = eval_binop(n, &l, &r);
            last_eval_type = TYPE_BOOL;
            if (ops == OPS_STR) {
                long c = strcmp(sbuf_l, sbuf_r);
                l = c;
                r = 0;
                binop_ltype = binop_rtype = TYPE_INT;
            } else if (ops != OPS_NUM) {
                raise_error("TypeError: order");
                return 0;
            }
            if (BINOP_FLOAT) {
                int8_t c2 = f32_cmp(num_to_f32(l, binop_ltype),
                                    num_to_f32(r, binop_rtype));
                if (n->type == AST_LT) return c2 < 0;
                if (n->type == AST_GT) return c2 > 0;
                if (n->type == AST_LE) return c2 <= 0;
                return c2 >= 0;
            }
            if (n->type == AST_LT) return l < r;
            if (n->type == AST_GT) return l > r;
            if (n->type == AST_LE) return l <= r;
            return l >= r;
        }
        case AST_AND: {
            /* Python semantics: return the deciding operand's value */
            long l = evaluate(n->left);
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
            long l = evaluate(n->left);
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
            long val = evaluate(n->right);
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
            long start = 0, stop = 0, step = 1;
            long v;
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
                long val = evaluate(n->left);
                emit_value(val, last_eval_type, last_eval_str_bank, 0);
            } else {
                sbuf_l[0] = '\0'; /* stage in WRAM: out_putline is banked */
                out_putline(sbuf_l);
            }
            last_eval_type = 0;
            return 0;
        }
        case AST_DEF:
            if (current_class != NULL) {
                MethodReg* m = find_method(current_class, n->identifier);
                if (m == NULL) {
                    m = (MethodReg*)malloc(sizeof(MethodReg));
                    if (m != NULL) {
                        strcpy(m->name, n->identifier);
                        m->next = current_class->methods;
                        current_class->methods = m;
                    }
                }
                if (m != NULL) m->def = n;
            } else {
                register_func(n->identifier, n);
            }
            last_eval_type = TYPE_NONE;
            return 0;
        case AST_CLASS: {
            ClassReg* c = find_class(n->identifier);
            if (c == NULL) {
                c = (ClassReg*)malloc(sizeof(ClassReg));
                if (c != NULL) {
                    strcpy(c->name, n->identifier);
                    c->methods = NULL;
                    c->next = classes;
                    classes = c;
                }
            }
            if (c != NULL) {
                current_class = c;
                if (n->right) evaluate(n->right);
                current_class = NULL;
            }
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_IMPORT: {
            ASTNode* mod = import_module(n->identifier);
            if (mod == NULL) {
                raise_error_name("ModuleNotFound", n->identifier);
                return 0;
            }
            evaluate(mod);
            last_eval_type = TYPE_NONE;
            return 0;
        }
        case AST_ATTR:
            return eval_attr(n);
        case AST_SETATTR:
            return eval_setattr(n);
        case AST_METHOD:
            return eval_method(n);
        case AST_CALL:
            return eval_call(n);
        
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
        case AST_INDEX:
            return eval_index(n);
        
        case AST_DICT:
            return eval_dict_lit(n);
        
        case AST_MULTI:
            return eval_multi(n);
        
        case AST_TUPLE: {
            long v2 = eval_list_like(n);
            last_eval_type = TYPE_TUPLE;
            return v2;
        }
        case AST_SET: {
            int d = dict_new();
            ASTNode* a = n->left;
            while (a && d) {
                long kv = evaluate(a->left);
                uint8_t kt = last_eval_type;
                if (kt == TYPE_STR) {
                    kv = store_str_value(kv, last_eval_str_bank);
                } else if (kt != TYPE_INT && kt != TYPE_BOOL) {
                    raise_error("TypeError: set elem");
                    return 0;
                }
                if (exec_signal == SIG_ERROR) return 0;
                dict_set(d, kt, kv, TYPE_NONE, 0);
                a = a->right;
            }
            last_eval_type = TYPE_SET;
            return d;
        }
        case AST_LIST:
            return eval_list_like(n);
        
        case AST_STORE:
            return eval_store(n);
        
        case AST_FORIN:
            return eval_forin(n);
        
        case AST_IN:
            return eval_in(n);
        
        case AST_SLICE:
            return eval_slice(n);
        
        case AST_CHAIN:
            return eval_chain(n);
        
        case AST_ELSE:
            return 0; /* handled inside AST_IF */
        case AST_SEQ: {
            long val = evaluate(n->left);
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

uint8_t call_depth = 0;
#define MAX_CALL_DEPTH 16

static long eval_call(ASTNode* n) {
            long argv[4];
            uint8_t arg_type[4];
            uint8_t arg_bank[4];
            uint8_t argc = 0;
            uint8_t i;
            ASTNode* a = n->left;
            FuncReg* f;

            while (a && argc < 4) {
                argv[argc] = evaluate(a->left);
                arg_type[argc] = last_eval_type;
                arg_bank[argc] = last_eval_str_bank;
                argc++;
                a = a->right;
            }

            if (exec_signal == SIG_ERROR) return 0;

            {
                long bres;
                if (call_builtin(n->identifier, argv, arg_type, arg_bank,
                                 argc, &bres)) {
                    return bres;
                }
            }

            f = find_func(n->identifier);
            if (f != NULL) {
                return invoke_function(f->def, n->identifier,
                                       argv, arg_type, arg_bank, argc);
            }
            {
                /* class instantiation: Name(args) */
                ClassReg* c = find_class(n->identifier);
                if (c != NULL) {
                    int inst = dict_new();
                    MethodReg* init;
                    if (!inst) return 0;
                    /* hidden class link: TYPE_NONE key can't collide */
                    dict_set(inst, TYPE_NONE, 0, TYPE_INT, (long)(uint16_t)c);
                    init = find_method(c, "__init__");
                    if (init != NULL) {
                        long margv[4];
                        uint8_t mat[4];
                        uint8_t mab[4];
                        margv[0] = inst;
                        mat[0] = TYPE_OBJ;
                        mab[0] = 2;
                        for (i = 0; i < argc && i < 3; i++) {
                            margv[i + 1] = argv[i];
                            mat[i + 1] = arg_type[i];
                            mab[i + 1] = arg_bank[i];
                        }
                        invoke_function(init->def, n->identifier,
                                        margv, mat, mab, argc + 1);
                        if (exec_signal == SIG_ERROR) return 0;
                    }
                    last_eval_type = TYPE_OBJ;
                    return inst;
                }
            }
            last_eval_type = TYPE_INT;
            raise_error_name("NameError", n->identifier);
            return 0;
        }

static long eval_forin(ASTNode* n) {
            long seq_val = evaluate(n->left);
            uint8_t stype = last_eval_type;
            uint8_t sbank = last_eval_str_bank;
            int len, i;
            uint8_t et;
            long ev;
            if (stype == TYPE_DICT || stype == TYPE_SET) {
                /* python: iterating a dict/set yields its keys/elements */
                len = dict_len(seq_val);
                for (i = 0; i < len; i++) {
                    uint8_t kt, vt2;
                    long kv, vv2;
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
            if (stype == TYPE_LIST || stype == TYPE_TUPLE) {
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
                char iter[STR_MAX + 1];
                char ch[2];
                fetch_str(iter, seq_val, sbank);
                len = strlen(iter);
                ch[1] = '\0';
                for (i = 0; i < len; i++) {
                    ch[0] = iter[i];
                    /* bind as a bank-1-style literal so it's copied to
                       the persistent arena by set_variable */
                    set_variable(n->identifier, (long)(uint16_t)ch, TYPE_STR, 1);
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

static long eval_slice(ASTNode* n) {
            long base, start, stop;
            int len;
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
            } else if (btype == TYPE_LIST || btype == TYPE_TUPLE) {
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
                uint8_t count = (uint8_t)(stop - start);
                SWITCH_RAM(2);
                dst = (char*)sram_str_alloc(count + 1);
                if (dst != NULL) {
                    for (i2 = 0; i2 < count; i2++) {
                        dst[i2] = sbuf_l[(int)start + i2];
                    }
                    dst[count] = '\0';
                }
                SWITCH_RAM(1);
                last_eval_type = TYPE_STR;
                last_eval_str_bank = 2;
                return dst != NULL ? (long)(uint16_t)dst : 0;
            }
            {
                int dst = list_new((int)(stop - start));
                int i2;
                uint8_t et;
                long ev;
                if (dst) {
                    for (i2 = 0; i2 < stop - start; i2++) {
                        ev = list_get((int)base, (int)start + i2, &et);
                        list_set(dst, i2, ev, et);
                    }
                }
                last_eval_type = btype; /* slicing a tuple gives a tuple */
                return dst;
            }
        }

static long eval_chain(ASTNode* n) {
            /* python comparison chain: each operand evaluates once,
               short-circuits on the first false link */
            long prev = evaluate(n->left);
            uint8_t pt = last_eval_type;
            uint8_t pb = last_eval_str_bank;
            ASTNode* link = n->right;
            last_eval_type = TYPE_BOOL;
            while (link) {
                long rv;
                uint8_t rt, rb, res;
                ASTNodeType op = (ASTNodeType)(link->number >> 1);
                uint8_t negate = (uint8_t)(link->number & 1);
                rv = evaluate(link->left);
                rt = last_eval_type;
                rb = last_eval_str_bank;
                if (exec_signal == SIG_ERROR) return 0;
                if (op == AST_IN) {
                    /* membership inside a chain */
                    if (rt == TYPE_LIST || rt == TYPE_TUPLE) {
                        res = list_contains((int)rv, prev, pt, pb);
                    } else if (rt == TYPE_DICT || rt == TYPE_SET) {
                        res = dict_find((int)rv, pt, prev, pb) >= 0;
                    } else {
                        raise_error("TypeError: in");
                        return 0;
                    }
                } else {
                    res = compare_op(op, pt, prev, pb, rt, rv, rb);
                }
                last_eval_type = TYPE_BOOL;
                if (exec_signal == SIG_ERROR) return 0;
                if (negate) res = !res;
                if (!res) return 0;
                prev = rv;
                pt = rt;
                pb = rb;
                link = link->right;
            }
            return 1;
        }

static long eval_dict_lit(ASTNode* n) {
            int d = dict_new();
            ASTNode* a = n->left;
            while (a && d) {
                long kv, vv;
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

static long eval_multi(ASTNode* n) {
            long vals[4];
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
            if (nv == 1 && nt > 1 &&
                (vts[0] == TYPE_LIST || vts[0] == TYPE_TUPLE)) {
                /* a, b = pair — unpack a sequence value */
                int sl = list_len(vals[0]);
                long sv = vals[0];
                uint8_t et;
                if (sl != nt) {
                    raise_error("ValueError: unpack");
                    return 0;
                }
                t = n->left;
                for (i = 0; i < nt; i++) {
                    long ev = list_get((int)sv, i, &et);
                    set_variable(t->identifier, ev, et, 2);
                    t = t->right;
                }
                last_eval_type = TYPE_NONE;
                return 0;
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

static long eval_store(ASTNode* n) {
            ASTNode* target = n->left; /* AST_INDEX node */
            long base, idx, val;
            int len;
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
            list_set((int)base, (int)idx, val, vt);
            return val;
        }

static long eval_index(ASTNode* n) {
            long base, idx;
            int len;
            uint8_t btype, bbank, ktype, kbank;
            base = evaluate(n->left);
            btype = last_eval_type;
            bbank = last_eval_str_bank;
            idx = evaluate(n->right);
            ktype = last_eval_type;
            kbank = last_eval_str_bank;
            if (exec_signal == SIG_ERROR) return 0;
            if (btype == TYPE_DICT) {
                int slot;
                uint8_t kt2, vt2;
                long kv2, vv2;
                if (ktype == TYPE_FLOAT) {
                    raise_error("TypeError: key");
                    return 0;
                }
                slot = dict_find(base, ktype, idx, kbank);
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
                    dst[0] = sbuf_l[(int)idx];
                    dst[1] = '\0';
                }
                SWITCH_RAM(1);
                last_eval_type = TYPE_STR;
                last_eval_str_bank = 2;
                return dst != NULL ? (long)(uint16_t)dst : 0;
            }
            if (btype == TYPE_LIST || btype == TYPE_TUPLE) {
                uint8_t et;
                long ev;
                len = list_len(base);
                if (idx < 0) idx += len;
                if (idx < 0 || idx >= len) {
                    raise_error("IndexError");
                    return 0;
                }
                ev = list_get(base, (int)idx, &et);
                last_eval_type = et;
                last_eval_str_bank = 2;
                return ev;
            }
            raise_error("TypeError: subscript");
            return 0;
        }

static long eval_in(ASTNode* n) {
            long l, r;
            uint8_t lt, lb, rt;
            l = evaluate(n->left);
            lt = last_eval_type;
            lb = last_eval_str_bank;
            r = evaluate(n->right);
            rt = last_eval_type;
            if (exec_signal == SIG_ERROR) return 0;
            last_eval_type = TYPE_BOOL;
            if (rt == TYPE_LIST || rt == TYPE_TUPLE) {
                return list_contains(r, l, lt, lb);
            }
            if (rt == TYPE_DICT || rt == TYPE_SET) {
                /* python: 'k' in d tests keys */
                return dict_find((int)r, lt, l, lb) >= 0;
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

static long eval_list_like(ASTNode* n) {
            int dst;
            int count = 0;
            int i = 0;
            long v;
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

static long invoke_function(ASTNode* def, const char* name, long* argv,
                            uint8_t* arg_type, uint8_t* arg_bank,
                            uint8_t argc) {
    ASTNode* param;
    EnvNode* saved_head;
    EnvNode* saved_frame;
    long result = 0;
    uint8_t i = 0;
    if (call_depth >= MAX_CALL_DEPTH) {
        last_eval_type = TYPE_INT;
        raise_error("RecursionError");
        return 0;
    }
    call_depth++;
    saved_head = env;
    saved_frame = frame_base;
    if (saved_frame == NULL) {
        globals_head = saved_head; /* frozen while calls run */
    }
    frame_base = saved_head;
    param = def->left;
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
        call_depth--;
        raise_error_name("TypeError", name);
        return 0;
    }
    if (def->right) {
        evaluate(def->right);
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
    while (env != saved_head) {
        EnvNode* t = env;
        env = env->next;
        free(t);
    }
    frame_base = saved_frame;
    call_depth--;
    return result;
}

static long eval_attr(ASTNode* n) {
    long base = evaluate(n->left);
    int slot;
    uint8_t kt2, vt2;
    long kv2, vv2;
    if (exec_signal == SIG_ERROR) return 0;
    if (last_eval_type != TYPE_OBJ) {
        raise_error("TypeError: attr");
        return 0;
    }
    /* attribute name is in the bank-1 AST */
    slot = dict_find((int)base, TYPE_STR, (long)(uint16_t)n->identifier, 1);
    if (slot < 0) {
        raise_error_name("AttributeError", n->identifier);
        return 0;
    }
    dict_entry((int)base, slot, &kt2, &kv2, &vt2, &vv2);
    last_eval_type = vt2;
    last_eval_str_bank = 2;
    return vv2;
}

static long eval_setattr(ASTNode* n) {
    ASTNode* target = n->left; /* AST_ATTR */
    long base = evaluate(target->left);
    long key, val;
    uint8_t vt;
    if (last_eval_type != TYPE_OBJ) {
        raise_error("TypeError: attr");
        return 0;
    }
    val = evaluate(n->right);
    vt = last_eval_type;
    if (exec_signal == SIG_ERROR) return 0;
    if (vt == TYPE_STR) {
        val = store_str_value(val, last_eval_str_bank);
    }
    key = store_str_value((long)(uint16_t)target->identifier, 1);
    dict_set((int)base, TYPE_STR, key, vt, val);
    last_eval_type = TYPE_NONE;
    return 0;
}

static long eval_method(ASTNode* n) {
    long argv[4];
    uint8_t arg_type[4];
    uint8_t arg_bank[4];
    uint8_t argc = 1;
    ASTNode* a = n->right;
    ClassReg* c;
    MethodReg* m;
    uint8_t kt2, vt2;
    long kv2, vv2;

    argv[0] = evaluate(n->left); /* self */
    arg_type[0] = TYPE_OBJ;
    arg_bank[0] = 2;
    if (exec_signal == SIG_ERROR) return 0;
    if (last_eval_type != TYPE_OBJ) {
        raise_error("TypeError: method");
        return 0;
    }
    while (a && argc < 4) {
        argv[argc] = evaluate(a->left);
        arg_type[argc] = last_eval_type;
        arg_bank[argc] = last_eval_str_bank;
        argc++;
        a = a->right;
    }
    if (exec_signal == SIG_ERROR) return 0;
    /* entry 0 is the hidden class link (TYPE_NONE key) */
    dict_entry((int)argv[0], 0, &kt2, &kv2, &vt2, &vv2);
    if (kt2 != TYPE_NONE) {
        raise_error("TypeError: method");
        return 0;
    }
    c = (ClassReg*)(uint16_t)vv2;
    m = find_method(c, n->identifier);
    if (m == NULL) {
        raise_error_name("AttributeError", n->identifier);
        return 0;
    }
    return invoke_function(m->def, n->identifier, argv, arg_type,
                           arg_bank, argc);
}

/* Top-level execution: python REPL semantics. Expression statements echo
   their value; assignments, compound statements, and print() do not. */
void exec_statement(ASTNode* n) {
    long res;
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
        case AST_CLASS:
        case AST_IMPORT:
        case AST_SETATTR:
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
    classes_clear();
    current_class = NULL;
    frame_base = NULL;
    globals_head = NULL;
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
    call_depth = 0;

    /* Enable SRAM and switch to Bank 1 for parsing and evaluation */
    ENABLE_RAM;
    SWITCH_RAM(1);

    lexer_reset(input_buffer);
    next_token();

    ast = parse_program();
    if (exec_signal == SIG_ERROR) {
        /* arena filled during parse: wipe everything */
        out_putline(err_buf);
        raise_error("(state cleared)");
        out_putline(err_buf);
        exec_signal = SIG_NONE;
        full_reset();
    } else if (ast) {
        exec_statement(ast);
        if (exec_signal == SIG_ERROR) {
            out_putline(err_buf);
            exec_signal = SIG_NONE;
            if (strcmp(err_buf, "MemoryError") == 0) {
                raise_error("(state cleared)");
                out_putline(err_buf);
                exec_signal = SIG_NONE;
                full_reset();
            }
        }
    } else if (input_len > 0) {
        raise_error("SyntaxError"); /* stages the text in WRAM err_buf */
        out_putline(err_buf);
        exec_signal = SIG_NONE;
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

