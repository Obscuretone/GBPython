/* gbpython lexer: no SRAM access, lives in always-mapped ROM bank 0 so
   the bank-1 parser can call it directly. */

#include <string.h>
#include <ctype.h>
#include "gbpython.h"

/* Lexer Definitions */


const char* src_ptr;
Token curr_tok;

/* Indentation tracking: python-style INDENT/DEDENT tokens */
#define MAX_INDENT 8
uint8_t indent_stack[MAX_INDENT] = {0};
uint8_t indent_sp = 0;
uint8_t pending_dedents = 0;
uint8_t at_line_start = 1;

void lexer_reset(const char* src) {
    src_ptr = src;
    indent_stack[0] = 0;
    indent_sp = 0;
    pending_dedents = 0;
    at_line_start = 1;
}

static TokenType keyword_type(const char* text) {
    if (strcmp(text, "while") == 0) return TOK_WHILE;
    if (strcmp(text, "if") == 0) return TOK_IF;
    if (strcmp(text, "elif") == 0) return TOK_ELIF;
    if (strcmp(text, "else") == 0) return TOK_ELSE;
    if (strcmp(text, "for") == 0) return TOK_FOR;
    if (strcmp(text, "in") == 0) return TOK_IN;
    if (strcmp(text, "range") == 0) return TOK_RANGE;
    if (strcmp(text, "print") == 0) return TOK_PRINT;
    if (strcmp(text, "and") == 0) return TOK_AND;
    if (strcmp(text, "or") == 0) return TOK_OR;
    if (strcmp(text, "not") == 0) return TOK_NOT;
    if (strcmp(text, "def") == 0) return TOK_DEF;
    if (strcmp(text, "return") == 0) return TOK_RETURN;
    if (strcmp(text, "break") == 0) return TOK_BREAK;
    if (strcmp(text, "continue") == 0) return TOK_CONTINUE;
    if (strcmp(text, "pass") == 0) return TOK_PASS;
    return TOK_IDENTIFIER;
}

void next_token(void) {
    uint8_t i;

    if (pending_dedents) {
        pending_dedents--;
        curr_tok.type = TOK_DEDENT;
        return;
    }

    if (at_line_start) {
        uint8_t col;
        /* Measure indentation; skip blank and comment-only lines entirely */
        for (;;) {
            col = 0;
            while (*src_ptr == ' ' || *src_ptr == '\r') {
                if (*src_ptr == ' ') col++;
                src_ptr++;
            }
            if (*src_ptr == '#') {
                while (*src_ptr && *src_ptr != '\n') src_ptr++;
            }
            if (*src_ptr == '\n') {
                src_ptr++;
                continue;
            }
            break;
        }
        at_line_start = 0;
        if (*src_ptr == '\0') {
            if (indent_sp > 0) {
                pending_dedents = indent_sp - 1;
                indent_sp = 0;
                curr_tok.type = TOK_DEDENT;
                return;
            }
            curr_tok.type = TOK_EOF;
            return;
        }
        if (col > indent_stack[indent_sp]) {
            if (indent_sp < MAX_INDENT - 1) {
                indent_sp++;
                indent_stack[indent_sp] = col;
            }
            curr_tok.type = TOK_INDENT;
            return;
        }
        while (col < indent_stack[indent_sp] && indent_sp > 0) {
            indent_sp--;
            pending_dedents++;
        }
        if (pending_dedents) {
            pending_dedents--;
            curr_tok.type = TOK_DEDENT;
            return;
        }
        /* same indent level: fall through to normal token */
    }

    while (*src_ptr == ' ' || *src_ptr == '\r') {
        src_ptr++;
    }
    if (*src_ptr == '#') {
        while (*src_ptr && *src_ptr != '\n') src_ptr++;
    }

    if (*src_ptr == '\0') {
        if (indent_sp > 0) {
            pending_dedents = indent_sp - 1;
            indent_sp = 0;
            curr_tok.type = TOK_DEDENT;
            return;
        }
        curr_tok.type = TOK_EOF;
        return;
    }
    if (*src_ptr == '\n') {
        curr_tok.type = TOK_NEWLINE;
        src_ptr++;
        at_line_start = 1;
        return;
    }
    if (*src_ptr == ';') {
        curr_tok.type = TOK_SEMICOLON;
        src_ptr++;
        return;
    }
    if (*src_ptr == ':') {
        curr_tok.type = TOK_COLON;
        src_ptr++;
        return;
    }
    if (*src_ptr == ',') {
        curr_tok.type = TOK_COMMA;
        src_ptr++;
        return;
    }
    if (*src_ptr == '+') {
        curr_tok.type = TOK_PLUS;
        src_ptr++;
        return;
    }
    if (*src_ptr == '-') {
        curr_tok.type = TOK_MINUS;
        src_ptr++;
        return;
    }
    if (*src_ptr == '*') {
        curr_tok.type = TOK_MUL;
        src_ptr++;
        return;
    }
    if (*src_ptr == '/') {
        src_ptr++;
        if (*src_ptr == '/') {
            src_ptr++;
            curr_tok.type = TOK_FLOORDIV;
            return;
        }
        curr_tok.type = TOK_DIV;
        return;
    }
    if (*src_ptr == '%') {
        curr_tok.type = TOK_MOD;
        src_ptr++;
        return;
    }
    if (*src_ptr == '(') {
        curr_tok.type = TOK_LPAREN;
        src_ptr++;
        return;
    }
    if (*src_ptr == ')') {
        curr_tok.type = TOK_RPAREN;
        src_ptr++;
        return;
    }
    if (*src_ptr == '[') {
        curr_tok.type = TOK_LBRACKET;
        src_ptr++;
        return;
    }
    if (*src_ptr == ']') {
        curr_tok.type = TOK_RBRACKET;
        src_ptr++;
        return;
    }
    if (*src_ptr == '{') {
        curr_tok.type = TOK_LBRACE;
        src_ptr++;
        return;
    }
    if (*src_ptr == '}') {
        curr_tok.type = TOK_RBRACE;
        src_ptr++;
        return;
    }
    if (*src_ptr == '<') {
        src_ptr++;
        if (*src_ptr == '=') {
            curr_tok.type = TOK_LE;
            src_ptr++;
            return;
        }
        curr_tok.type = TOK_LT;
        return;
    }
    if (*src_ptr == '>') {
        src_ptr++;
        if (*src_ptr == '=') {
            curr_tok.type = TOK_GE;
            src_ptr++;
            return;
        }
        curr_tok.type = TOK_GT;
        return;
    }
    if (*src_ptr == '!') {
        src_ptr++;
        if (*src_ptr == '=') {
            curr_tok.type = TOK_NEQ;
            src_ptr++;
            return;
        }
    }
    if (*src_ptr == '=') {
        src_ptr++;
        if (*src_ptr == '=') {
            curr_tok.type = TOK_EQEQ;
            src_ptr++;
            return;
        }
        curr_tok.type = TOK_EQUAL;
        return;
    }

    if (*src_ptr == '\'' || *src_ptr == '"') {
        char quote = *src_ptr;
        src_ptr++; /* skip opening quote */
        curr_tok.type = TOK_STRING;
        i = 0;
        while (*src_ptr != quote && *src_ptr != '\0' && i < 15) {
            curr_tok.text[i++] = *src_ptr++;
        }
        curr_tok.text[i] = '\0';
        if (*src_ptr == quote) {
            src_ptr++; /* skip closing quote */
        }
        return;
    }

    if (isdigit(*src_ptr)) {
        curr_tok.type = TOK_NUMBER;
        curr_tok.value = 0;
        curr_tok.is_float = 0;
        while (isdigit(*src_ptr)) {
            curr_tok.value = curr_tok.value * 10 + (*src_ptr - '0');
            src_ptr++;
        }
        if (*src_ptr == '.' && isdigit(src_ptr[1])) {
            long f = f32_from_int(curr_tok.value);
            long ten = f32_from_int(10);
            long scale = f32_div(f32_from_int(1), ten);
            src_ptr++;
            while (isdigit(*src_ptr)) {
                f = f32_add(f, f32_mul(f32_from_int(*src_ptr - '0'), scale));
                scale = f32_div(scale, ten);
                src_ptr++;
            }
            curr_tok.is_float = 1;
            curr_tok.value = f;
        }
        return;
    }

    if (*src_ptr == '.') {
        curr_tok.type = TOK_DOT;
        src_ptr++;
        return;
    }

    if (isalpha(*src_ptr) || *src_ptr == '_') {
        curr_tok.type = TOK_IDENTIFIER;
        i = 0;
        while ((isalpha(*src_ptr) || isdigit(*src_ptr) || *src_ptr == '_') && i < 15) {
            curr_tok.text[i++] = *src_ptr++;
        }
        curr_tok.text[i] = '\0';
        if (strcmp(curr_tok.text, "True") == 0) {
            curr_tok.type = TOK_TRUE;
            return;
        }
        if (strcmp(curr_tok.text, "False") == 0) {
            curr_tok.type = TOK_FALSE;
            return;
        }
        if (strcmp(curr_tok.text, "None") == 0) {
            curr_tok.type = TOK_NONE;
            return;
        }
        curr_tok.type = keyword_type(curr_tok.text);
        return;
    }

    src_ptr++;
    next_token();
}

