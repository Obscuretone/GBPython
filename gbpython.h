#ifndef GBPYTHON_H
#define GBPYTHON_H

#include <gb/gb.h>
#include <stdint.h>

#define INPUT_MAX 254

/* UI state and helpers (gbpython.c, ROM bank 0) */
extern char input_buffer[INPUT_MAX + 1];
extern uint8_t input_len;
extern uint8_t out_count;
void draw_input_buffer(void);

/* Interactive line entry on the on-screen keyboard, used by input().
   Writes up to maxlen chars + NUL into dst. */
void ui_input_line(char* dst, uint8_t maxlen);

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
    TOK_DIV,
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
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT
} TokenType;

typedef struct {
    TokenType type;
    char text[16];
    int value;
} Token;

extern const char* src_ptr;
extern Token curr_tok;
void lexer_reset(const char* src);
void next_token(void);

/* Runtime (runtime.c, ROM bank 0): arenas, values, output, errors */

/* Value types */
#define TYPE_INT 0
#define TYPE_STR 1
#define TYPE_LIST 2
#define TYPE_BOOL 3
#define TYPE_NONE 4
#define TYPE_DICT 5

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
extern int return_value;
extern uint8_t return_type;
extern uint8_t return_str_bank;
extern char err_buf[28];
void raise_error(const char* msg);
void raise_error_name(const char* kind, const char* name);
void raise_memory_error(void);

extern char sbuf_l[33];
extern char sbuf_r[33];
void fetch_str(char* dst, int val, uint8_t bank);
int store_str_value(int val, uint8_t str_bank);
int str_concat(void);

int list_len(int ptr);
int list_get(int ptr, int i, uint8_t* t);
void list_set(int ptr, int i, int v, uint8_t t);
int list_new(int len);
uint8_t list_eq(int a, int b);
uint8_t list_contains(int ptr, int lv, uint8_t lt, uint8_t lbank);

int dict_new(void);
int dict_len(int d);
void dict_entry(int d, int i, uint8_t* kt, int* kv, uint8_t* vt, int* vv);
int dict_find(int d, uint8_t kt, int kv, uint8_t kbank);
uint8_t dict_set(int d, uint8_t kt, int kv, uint8_t vt, int vv);

void out_redraw(void);
void out_putline(const char* s);
extern char line_buf[24];
void render_list(int ptr, char* buf);
void emit_value(int val, uint8_t vtype, uint8_t str_bank, uint8_t echo);
uint8_t truthy(int val, uint8_t vtype, uint8_t str_bank);

#endif
