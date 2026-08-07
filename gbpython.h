#ifndef GBPYTHON_H
#define GBPYTHON_H

#include <gb/gb.h>
#include <stdint.h>

#define INPUT_MAX 254

/* UI state and helpers (gbpython.c, ROM bank 0) */
extern char input_buffer[INPUT_MAX + 1];
extern uint8_t input_len;
extern uint8_t out_count;

#define OSK_ROWS 4
#define OSK_COLS 15
#define OSK_X 2
#define OSK_Y 13
extern char osk_grid[OSK_ROWS][OSK_COLS];
extern uint8_t cursor_row;
extern uint8_t cursor_col;
void draw_osk(void);

/* Banked UI (ui.c, ROM bank 3): splash, input editor, input() line entry */
void splash_screen(void) BANKED;
void draw_input_buffer(void) BANKED;
void ui_input_line(char* dst, uint8_t maxlen) BANKED;

/* Builtins dispatcher (builtins.c, ROM bank 3): returns 1 when name is a
   builtin (result and value channel set), 0 to try user functions */
uint8_t call_builtin(const char* name, long* argv, uint8_t* arg_type,
                     uint8_t* arg_bank, uint8_t argc, long* result) BANKED;

/* Interpreter entry point (interpreter.c, ROM bank 1) */
void run_interpreter(void) BANKED;

/* Lexer (lexer.c, ROM bank 0 — no SRAM access, callable from any bank) */
typedef enum {
    TOK_EOF = 0,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_EQUAL,
    TOK_PLUS,
    TOK_MINUS,
    TOK_MUL,
    TOK_DIV,      /* '/'  true division  */
    TOK_FLOORDIV, /* '//' floor division */
    TOK_MOD,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_COMMA,
    TOK_WHILE,
    TOK_IF,
    TOK_ELIF,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_RANGE,
    TOK_PRINT,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,
    TOK_DEF,
    TOK_RETURN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_PASS,
    TOK_CLASS,
    TOK_IMPORT,
    TOK_GLOBAL,
    TOK_TRY,
    TOK_EXCEPT,
    TOK_RAISE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_DOT,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT
} TokenType;

#define NAME_MAX 19 /* fits ZeroDivisionError */

typedef struct {
    TokenType type;
    char text[NAME_MAX + 1];
    long value;      /* int value, or float bits when is_float */
    uint8_t is_float;
} Token;

extern const char* src_ptr;
extern Token curr_tok;
void lexer_reset(const char* src);
void next_token(void);

/* AST (nodes built by parser.c in ROM bank 2, allocated by make_node in
   runtime.c, evaluated by interpreter.c in ROM bank 1) */
typedef enum {
    AST_NUMBER,
    AST_IDENTIFIER,
    AST_STRING,
    AST_ADD,
    AST_SUB,
    AST_MUL,
    AST_DIV,     /* floor division (//)   */
    AST_TRUEDIV, /* true division (/), floats */
    AST_MOD,
    AST_NEG,
    AST_EQEQ,
    AST_NEQ,
    AST_LT,
    AST_GT,
    AST_LE,
    AST_GE,
    AST_AND,
    AST_OR,
    AST_NOT,
    AST_ASSIGN,
    AST_WHILE,
    AST_IF,     /* left = condition, right = AST_ELSE pair */
    AST_ELSE,   /* left = then-block, right = else-block (or NULL) */
    AST_FOR,    /* identifier = loop var, left = AST_RANGE, right = body */
    AST_RANGE,  /* left = first arg, right = NULL or AST_RANGE(stop, step) */
    AST_PRINT,  /* left = expression or NULL */
    AST_DEF,    /* identifier = name, left = param chain, right = body */
    AST_PARAM,  /* identifier = param name, right = next param */
    AST_CALL,   /* identifier = name, left = arg chain */
    AST_ARG,    /* left = expression, right = next arg */
    AST_RETURN, /* left = expression or NULL */
    AST_BREAK,
    AST_CONTINUE,
    AST_PASS,
    AST_INDEX,  /* left = base expression, right = index expression */
    AST_LIST,   /* left = AST_ARG element chain */
    AST_STORE,  /* left = AST_INDEX target, right = value expression */
    AST_FORIN,  /* identifier = loop var, left = iterable expr, right = body */
    AST_BOOL,   /* number = 0 or 1 */
    AST_NONE,
    AST_FLOAT,  /* number = IEEE-754 bits */
    AST_IN,     /* left = needle, right = haystack (list or string) */
    AST_SLICE,  /* left = base, right = AST_ARG(start-or-NULL, stop-or-NULL) */
    AST_DICT,   /* left = AST_ARG chain, alternating key, value, key, ... */
    AST_MULTI,  /* left = AST_PARAM target chain, right = AST_ARG value chain */
    AST_CHAIN,  /* left = first operand; right = ARG links (number = op*2+negate) */
    AST_TUPLE,  /* left = AST_ARG element chain */
    AST_SET,    /* left = AST_ARG element chain */
    AST_CLASS,  /* identifier = name, right = body of defs */
    AST_ATTR,   /* left = base expr, identifier = attribute name */
    AST_SETATTR,/* left = AST_ATTR target, right = value */
    AST_METHOD, /* left = base expr, identifier = method, right = arg chain */
    AST_IMPORT, /* identifier = module name */
    AST_GLOBAL, /* left = AST_PARAM name chain */
    AST_TRY,    /* left = body, right = AST_EXCEPT chain */
    AST_EXCEPT, /* identifier = filter ('' = bare), left = handler, right = next */
    AST_RAISE,  /* identifier = error name, left = optional message expr */
    AST_SEQ
} ASTNodeType;

/* identifier sits at the tail: node types that don't carry a name are
   allocated short (make_node), which more than doubles arena capacity —
   most nodes are operators and sequence links. */
typedef struct ASTNode {
    uint8_t type; /* ASTNodeType */
    long number;
    struct ASTNode* left;
    struct ASTNode* right;
    char identifier[NAME_MAX + 1];
} ASTNode;

#define AST_NODE_SHORT (sizeof(ASTNode) - (NAME_MAX + 1))

ASTNode* make_node(ASTNodeType type);
ASTNode* parse_program(void) BANKED;
/* Parse source text (module import); does its own lexer_reset */
ASTNode* parse_module(const char* src) BANKED;
/* Find + parse a ROM-baked module; NULL if unknown */
ASTNode* import_module(const char* name) BANKED;

/* Runtime (runtime.c, ROM bank 0): arenas, values, output, errors */

/* Value types */
#define TYPE_INT 0
#define TYPE_STR 1
#define TYPE_LIST 2
#define TYPE_BOOL 3
#define TYPE_NONE 4
#define TYPE_DICT 5
#define TYPE_FLOAT 6
#define TYPE_TUPLE 7  /* list storage, immutable */
#define TYPE_OBJ 8    /* dict storage + hidden class link */
#define TYPE_SET 9    /* dict storage, values are None */

/* Control-flow signals */
#define SIG_NONE 0
#define SIG_BREAK 1
#define SIG_CONTINUE 2
#define SIG_RETURN 3
#define SIG_ERROR 4

extern uint8_t* sram_ast_ptr;
extern uint8_t* sram_def_ptr;
extern uint8_t def_mode;
extern uint8_t* sram_str_ptr;
extern uint8_t* sram_list_ptr;
void* sram_ast_alloc(size_t size);
void* sram_str_alloc(size_t size);
void* sram_list_alloc(size_t size);
void sram_ast_reset(void);

extern uint8_t last_eval_type;
extern uint8_t last_eval_str_bank;
extern uint8_t exec_signal;
extern long return_value;
extern uint8_t return_type;
extern uint8_t return_str_bank;
extern char err_buf[40];
void raise_error(const char* msg);
void raise_error_name(const char* kind, const char* name);
void raise_memory_error(void);

#define STR_MAX 127
extern char sbuf_l[STR_MAX + 1];
extern char sbuf_r[STR_MAX + 1];
void fetch_str(char* dst, long val, uint8_t bank);
long store_str_value(long val, uint8_t str_bank);
long str_concat(void);

/* Soft-float (float32.c, bank 0): IEEE-754 single precision operations on
   raw bit patterns carried in the 32-bit value slot */
uint8_t f32_is_zero(long a) BANKED;
long f32_neg(long a) BANKED;
long f32_from_int(long v) BANKED;
long f32_trunc(long a) BANKED;
long f32_floor(long a) BANKED;
int8_t f32_cmp(long a, long b) BANKED;
long f32_add(long a, long b) BANKED;
long f32_sub(long a, long b) BANKED;
long f32_mul(long a, long b) BANKED;
long f32_div(long a, long b) BANKED;
/* value (int/bool/float) -> float bits */
long num_to_f32(long v, uint8_t t);
void render_float(long bits, char* buf) BANKED;

int list_len(int ptr);
long list_get(int ptr, int i, uint8_t* t);
void list_set(int ptr, int i, long v, uint8_t t);
int list_new(int len);
uint8_t num_eq(uint8_t ta, long va, uint8_t tb, long vb) BANKED;
uint8_t val_eq(uint8_t ta, long va, uint8_t tb, long vb) BANKED;
uint8_t list_eq(int a, int b) BANKED;
uint8_t dict_eq(int a, int b) BANKED;
uint8_t list_contains(int ptr, long lv, uint8_t lt, uint8_t lbank) BANKED;

int dict_new(void);
int dict_len(int d);
void dict_entry(int d, int i, uint8_t* kt, long* kv, uint8_t* vt, long* vv);
int dict_find(int d, uint8_t kt, long kv, uint8_t kbank);
uint8_t dict_set(int d, uint8_t kt, long kv, uint8_t vt, long vv);

void out_redraw(void) BANKED;
void out_putline(const char* s) BANKED;
extern char line_buf[24];
void render_list(int ptr, char* buf) BANKED;
void render_seq_inner(int ptr, char* buf, uint8_t* pos, uint8_t tuple) BANKED;
void render_dict_inner(int d, char* buf, uint8_t* pos, uint8_t is_set) BANKED;
void emit_value(long val, uint8_t vtype, uint8_t str_bank, uint8_t echo) BANKED;
uint8_t truthy(long val, uint8_t vtype, uint8_t str_bank);

#endif
