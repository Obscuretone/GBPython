/* gbpython parser: recursive descent over the bank-0 lexer, building the
   AST in banked SRAM via make_node (bank 0). Lives in ROM bank 2; the
   only cross-bank entry point is parse_program (BANKED). */

#pragma bank 2

#include <gb/gb.h>
#include <string.h>
#include <ctype.h>
#include "gbpython.h"

/* Parser */
ASTNode* parse_expression(void);
ASTNode* parse_statement(void);
ASTNode* parse_suite(void);

ASTNode* parse_primary(void) {
    if (curr_tok.type == TOK_TRUE || curr_tok.type == TOK_FALSE) {
        ASTNode* n = make_node(AST_BOOL);
        if (n != NULL) {
            n->number = (curr_tok.type == TOK_TRUE);
        }
        next_token();
        return n;
    }
    if (curr_tok.type == TOK_NONE) {
        next_token();
        return make_node(AST_NONE);
    }
    if (curr_tok.type == TOK_NUMBER) {
        ASTNode* n = make_node(curr_tok.is_float ? AST_FLOAT : AST_NUMBER);
        if (n != NULL) {
            n->number = curr_tok.value;
        }
        next_token();
        return n;
    }
    if (curr_tok.type == TOK_STRING) {
        ASTNode* n = make_node(AST_STRING);
        if (n != NULL) {
            strcpy(n->identifier, curr_tok.text);
        }
        next_token();
        return n;
    }
    if (curr_tok.type == TOK_IDENTIFIER) {
        ASTNode* n = make_node(AST_IDENTIFIER);
        if (n != NULL) {
            strcpy(n->identifier, curr_tok.text);
        }
        next_token();
        /* call syntax: name(arg, ...) — user functions and builtins */
        if (curr_tok.type == TOK_LPAREN && n != NULL) {
            ASTNode* arg_tail = NULL;
            n->type = AST_CALL;
            next_token();
            while (curr_tok.type != TOK_RPAREN && curr_tok.type != TOK_EOF &&
                   curr_tok.type != TOK_NEWLINE) {
                ASTNode* a = make_node(AST_ARG);
                if (a != NULL) {
                    a->left = parse_expression();
                }
                if (arg_tail == NULL) {
                    n->left = a;
                } else {
                    arg_tail->right = a;
                }
                arg_tail = a;
                if (curr_tok.type == TOK_COMMA) {
                    next_token();
                } else {
                    break;
                }
            }
            if (curr_tok.type == TOK_RPAREN) {
                next_token();
            }
        }
        return n;
    }
    if (curr_tok.type == TOK_LPAREN) {
        ASTNode* expr;
        next_token(); /* skip '(' */
        if (curr_tok.type == TOK_RPAREN) {
            next_token();
            return make_node(AST_TUPLE); /* () empty tuple */
        }
        expr = parse_expression();
        if (curr_tok.type == TOK_COMMA) {
            /* tuple literal (a, b, ...) — possibly with trailing comma */
            ASTNode* n = make_node(AST_TUPLE);
            ASTNode* first = make_node(AST_ARG);
            ASTNode* tail = first;
            if (first != NULL) {
                first->left = expr;
            }
            if (n != NULL) n->left = first;
            next_token();
            while (curr_tok.type != TOK_RPAREN && curr_tok.type != TOK_EOF &&
                   curr_tok.type != TOK_NEWLINE) {
                ASTNode* a = make_node(AST_ARG);
                if (a != NULL) {
                    a->left = parse_expression();
                }
                if (tail != NULL) tail->right = a;
                tail = a;
                if (curr_tok.type == TOK_COMMA) {
                    next_token();
                } else {
                    break;
                }
            }
            if (curr_tok.type == TOK_RPAREN) {
                next_token();
            }
            return n;
        }
        if (curr_tok.type == TOK_RPAREN) {
            next_token(); /* skip ')' */
        }
        return expr;
    }
    if (curr_tok.type == TOK_LBRACKET) {
        /* list literal [e1, e2, ...] */
        ASTNode* n = make_node(AST_LIST);
        ASTNode* tail = NULL;
        next_token();
        while (curr_tok.type != TOK_RBRACKET && curr_tok.type != TOK_EOF &&
               curr_tok.type != TOK_NEWLINE) {
            ASTNode* a = make_node(AST_ARG);
            if (a != NULL) {
                a->left = parse_expression();
            }
            if (tail == NULL) {
                if (n != NULL) n->left = a;
            } else {
                tail->right = a;
            }
            tail = a;
            if (curr_tok.type == TOK_COMMA) {
                next_token();
            } else {
                break;
            }
        }
        if (curr_tok.type == TOK_RBRACKET) {
            next_token();
        }
        return n;
    }
    if (curr_tok.type == TOK_LBRACE) {
        /* dict literal {k: v, ...} — entries flattened into an ARG chain */
        ASTNode* n = make_node(AST_DICT);
        ASTNode* tail = NULL;
        next_token();
        while (curr_tok.type != TOK_RBRACE && curr_tok.type != TOK_EOF &&
               curr_tok.type != TOK_NEWLINE) {
            ASTNode* k = make_node(AST_ARG);
            ASTNode* v;
            if (k != NULL) {
                k->left = parse_expression();
            }
            if (curr_tok.type != TOK_COLON && n != NULL && n->left == NULL) {
                /* {1, 2, 3} is a set: elements without values */
                n->type = AST_SET;
            }
            if (n != NULL && n->type == AST_SET) {
                if (n->left == NULL) {
                    n->left = k;
                } else {
                    tail->right = k;
                }
                tail = k;
                if (curr_tok.type == TOK_COMMA) {
                    next_token();
                    continue;
                }
                break;
            }
            v = make_node(AST_ARG);
            if (k != NULL) {
                k->right = v;
            }
            if (curr_tok.type == TOK_COLON) {
                next_token();
            }
            if (v != NULL) {
                v->left = parse_expression();
            }
            if (tail == NULL) {
                if (n != NULL) n->left = k;
            } else {
                tail->right = k;
            }
            tail = v;
            if (curr_tok.type == TOK_COMMA) {
                next_token();
            } else {
                break;
            }
        }
        if (curr_tok.type == TOK_RBRACE) {
            next_token();
        }
        return n;
    }
    return NULL;
}

/* Postfix subscripts base[i], slices base[a:b], attributes base.attr,
   and method calls base.m(args) */
ASTNode* parse_postfix(void) {
    ASTNode* base = parse_primary();
    while (curr_tok.type == TOK_LBRACKET || curr_tok.type == TOK_DOT) {
        if (curr_tok.type == TOK_DOT) {
            ASTNode* n;
            char attr[16];
            next_token();
            if (curr_tok.type != TOK_IDENTIFIER) return base;
            strcpy(attr, curr_tok.text);
            next_token();
            if (curr_tok.type == TOK_LPAREN) {
                ASTNode* tail = NULL;
                n = make_node(AST_METHOD);
                if (n != NULL) {
                    strcpy(n->identifier, attr);
                    n->left = base;
                }
                next_token();
                while (curr_tok.type != TOK_RPAREN &&
                       curr_tok.type != TOK_EOF &&
                       curr_tok.type != TOK_NEWLINE) {
                    ASTNode* a = make_node(AST_ARG);
                    if (a != NULL) {
                        a->left = parse_expression();
                    }
                    if (tail == NULL) {
                        if (n != NULL) n->right = a;
                    } else {
                        tail->right = a;
                    }
                    tail = a;
                    if (curr_tok.type == TOK_COMMA) {
                        next_token();
                    } else {
                        break;
                    }
                }
                if (curr_tok.type == TOK_RPAREN) {
                    next_token();
                }
            } else {
                n = make_node(AST_ATTR);
                if (n != NULL) {
                    strcpy(n->identifier, attr);
                    n->left = base;
                }
            }
            base = n;
            continue;
        }
        {
        ASTNode* n;
        ASTNode* start = NULL;
        next_token();
        if (curr_tok.type != TOK_COLON) {
            start = parse_expression();
        }
        if (curr_tok.type == TOK_COLON) {
            ASTNode* bounds;
            next_token();
            n = make_node(AST_SLICE);
            bounds = make_node(AST_ARG);
            if (bounds != NULL) {
                bounds->left = start;
                if (curr_tok.type != TOK_RBRACKET) {
                    bounds->right = parse_expression();
                }
            }
            if (n != NULL) {
                n->left = base;
                n->right = bounds;
            }
        } else {
            n = make_node(AST_INDEX);
            if (n != NULL) {
                n->left = base;
                n->right = start;
            }
        }
        if (curr_tok.type == TOK_RBRACKET) {
            next_token();
        }
        base = n;
        }
    }
    return base;
}

ASTNode* parse_unary(void) {
    if (curr_tok.type == TOK_MINUS) {
        ASTNode* n;
        next_token();
        n = make_node(AST_NEG);
        if (n != NULL) {
            n->left = parse_unary();
        }
        return n;
    }
    if (curr_tok.type == TOK_PLUS) {
        next_token();
        return parse_unary();
    }
    return parse_postfix();
}

ASTNode* parse_multiplicative(void) {
    ASTNode* left = parse_unary();
    while (curr_tok.type == TOK_MUL || curr_tok.type == TOK_DIV ||
           curr_tok.type == TOK_FLOORDIV || curr_tok.type == TOK_MOD) {
        TokenType op = curr_tok.type;
        ASTNode* right;
        ASTNode* n;
        next_token();
        right = parse_unary();
        n = make_node(op == TOK_MUL ? AST_MUL
                      : (op == TOK_DIV ? AST_TRUEDIV
                         : (op == TOK_FLOORDIV ? AST_DIV : AST_MOD)));
        if (n != NULL) {
            n->left = left;
            n->right = right;
        }
        left = n;
    }
    return left;
}

ASTNode* parse_additive(void) {
    ASTNode* left = parse_multiplicative();
    while (curr_tok.type == TOK_PLUS || curr_tok.type == TOK_MINUS) {
        TokenType op = curr_tok.type;
        ASTNode* right;
        ASTNode* n;
        next_token();
        right = parse_multiplicative();
        n = make_node(op == TOK_PLUS ? AST_ADD : AST_SUB);
        if (n != NULL) {
            n->left = left;
            n->right = right;
        }
        left = n;
    }
    return left;
}

/* Comparison chains (a < b < c) become an AST_CHAIN: left = the first
   operand, right = a linked list of AST_ARG links, each holding the
   comparison op in ->number and the operand in ->left. The evaluator walks
   the links so every operand evaluates exactly once, short-circuiting like
   python. A single comparison keeps its plain node shape. */
ASTNode* parse_comparison(void) {
    ASTNode* left = parse_additive();
    ASTNode* chain = NULL;
    ASTNode* link_tail = NULL;
    ASTNode* single = NULL;
    for (;;) {
        TokenType op = curr_tok.type;
        ASTNodeType ast_op;
        ASTNode* right;
        ASTNode* n;
        uint8_t negate = 0;

        if (op == TOK_NOT) {
            /* 'not in' — only if 'in' follows; plain 'not' here ends the
               comparison (it belongs to an enclosing 'and'/'or'/'not') */
            const char* p = src_ptr;
            while (*p == ' ') p++;
            if (p[0] == 'i' && p[1] == 'n' &&
                !(isalpha(p[2]) || isdigit(p[2]) || p[2] == '_')) {
                src_ptr = p + 2;
                next_token();
                op = TOK_IN;
                negate = 1;
            } else {
                break;
            }
        } else if (op == TOK_IN) {
            next_token();
        } else if (op == TOK_EQEQ || op == TOK_NEQ || op == TOK_LT ||
                   op == TOK_GT || op == TOK_LE || op == TOK_GE) {
            next_token();
        } else {
            break;
        }

        right = parse_additive();

        if (op == TOK_IN) ast_op = AST_IN;
        else if (op == TOK_EQEQ) ast_op = AST_EQEQ;
        else if (op == TOK_NEQ) ast_op = AST_NEQ;
        else if (op == TOK_LT) ast_op = AST_LT;
        else if (op == TOK_GT) ast_op = AST_GT;
        else if (op == TOK_LE) ast_op = AST_LE;
        else ast_op = AST_GE;

        if (single == NULL && chain == NULL) {
            /* first comparison: plain node (the common case) */
            single = make_node(ast_op);
            if (single != NULL) {
                single->left = left;
                single->right = right;
            }
            if (negate) {
                ASTNode* neg = make_node(AST_NOT);
                if (neg != NULL) neg->left = single;
                single = neg;
            }
        } else {
            ASTNode* link;
            if (chain == NULL) {
                /* second op appears: upgrade the single node to a chain.
                   'not in' links keep their negation in the link flag. */
                ASTNode* first = single;
                uint8_t first_neg = 0;
                if (first->type == AST_NOT) {
                    first_neg = 1;
                    first = first->left;
                }
                chain = make_node(AST_CHAIN);
                link = make_node(AST_ARG);
                if (chain != NULL && link != NULL) {
                    chain->left = first->left;
                    chain->right = link;
                    link->number = (long)first->type * 2 + first_neg;
                    link->left = first->right;
                }
                link_tail = link;
                single = NULL;
            }
            link = make_node(AST_ARG);
            if (link != NULL && link_tail != NULL) {
                link->number = (long)ast_op * 2 + negate;
                link->left = right;
                link_tail->right = link;
            }
            link_tail = link;
        }
    }
    if (chain != NULL) return chain;
    return single != NULL ? single : left;
}

ASTNode* parse_not(void) {
    if (curr_tok.type == TOK_NOT) {
        ASTNode* n;
        next_token();
        n = make_node(AST_NOT);
        if (n != NULL) {
            n->left = parse_not();
        }
        return n;
    }
    return parse_comparison();
}

ASTNode* parse_and(void) {
    ASTNode* left = parse_not();
    while (curr_tok.type == TOK_AND) {
        ASTNode* n;
        next_token();
        n = make_node(AST_AND);
        if (n != NULL) {
            n->left = left;
            n->right = parse_not();
        }
        left = n;
    }
    return left;
}

ASTNode* parse_or(void) {
    ASTNode* left = parse_and();
    while (curr_tok.type == TOK_OR) {
        ASTNode* n;
        next_token();
        n = make_node(AST_OR);
        if (n != NULL) {
            n->left = left;
            n->right = parse_and();
        }
        left = n;
    }
    return left;
}

ASTNode* parse_expression(void) {
    return parse_or();
}

/* Simple (non-compound) statement: assignment, print(), flow keyword,
   or expression */
ASTNode* parse_simple(void) {
    if (curr_tok.type == TOK_PASS) {
        next_token();
        return make_node(AST_PASS);
    }
    if (curr_tok.type == TOK_BREAK) {
        next_token();
        return make_node(AST_BREAK);
    }
    if (curr_tok.type == TOK_CONTINUE) {
        next_token();
        return make_node(AST_CONTINUE);
    }
    if (curr_tok.type == TOK_RETURN) {
        ASTNode* n;
        next_token();
        n = make_node(AST_RETURN);
        if (n != NULL &&
            curr_tok.type != TOK_NEWLINE && curr_tok.type != TOK_EOF &&
            curr_tok.type != TOK_SEMICOLON && curr_tok.type != TOK_DEDENT) {
            n->left = parse_expression();
        }
        return n;
    }
    if (curr_tok.type == TOK_PRINT) {
        ASTNode* n;
        next_token();
        n = make_node(AST_PRINT);
        if (curr_tok.type == TOK_LPAREN) {
            next_token();
            if (curr_tok.type != TOK_RPAREN && n != NULL) {
                n->left = parse_expression();
            }
            if (curr_tok.type == TOK_RPAREN) {
                next_token();
            }
        }
        return n;
    }

    if (curr_tok.type == TOK_IDENTIFIER) {
        /* Peek past the identifier in the raw source: '=' (but not '==')
           makes this an assignment; 'op=' an augmented assignment; ','
           starts a multiple assignment (a, b = x, y). Avoids lexer state
           save/restore. */
        const char* p = src_ptr;
        while (*p == ' ') p++;
        if (*p == ',') {
            /* a, b, c = e1, e2, e3 (evaluates all RHS first: swap works) */
            ASTNode* n = make_node(AST_MULTI);
            ASTNode* t_tail = NULL;
            ASTNode* v_tail = NULL;
            for (;;) {
                ASTNode* t = make_node(AST_PARAM);
                if (t != NULL) {
                    strcpy(t->identifier, curr_tok.text);
                }
                if (t_tail == NULL) {
                    if (n != NULL) n->left = t;
                } else {
                    t_tail->right = t;
                }
                t_tail = t;
                next_token();
                if (curr_tok.type != TOK_COMMA) break;
                next_token();
                if (curr_tok.type != TOK_IDENTIFIER) return NULL;
            }
            if (curr_tok.type != TOK_EQUAL) return NULL;
            next_token();
            for (;;) {
                ASTNode* v = make_node(AST_ARG);
                if (v != NULL) {
                    v->left = parse_expression();
                }
                if (v_tail == NULL) {
                    if (n != NULL) n->right = v;
                } else {
                    v_tail->right = v;
                }
                v_tail = v;
                if (curr_tok.type != TOK_COMMA) break;
                next_token();
            }
            return n;
        }
        if (*p == '=' && p[1] != '=') {
            ASTNode* n;
            char id[16];
            strcpy(id, curr_tok.text);
            src_ptr = p + 1;
            next_token();
            n = make_node(AST_ASSIGN);
            if (n != NULL) {
                strcpy(n->identifier, id);
                n->right = parse_expression();
            }
            return n;
        }
        if (((*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%') &&
             p[1] == '=') ||
            (*p == '/' && p[1] == '/' && p[2] == '=')) {
            /* x op= e  desugars to  x = x op e */
            ASTNode* n;
            ASTNode* op_node;
            ASTNode* self_ref;
            ASTNodeType op_type;
            uint8_t oplen = 2;
            char id[16];
            strcpy(id, curr_tok.text);
            if (*p == '+') op_type = AST_ADD;
            else if (*p == '-') op_type = AST_SUB;
            else if (*p == '*') op_type = AST_MUL;
            else if (*p == '/' && p[1] == '/') { op_type = AST_DIV; oplen = 3; }
            else if (*p == '/') op_type = AST_TRUEDIV;
            else op_type = AST_MOD;
            src_ptr = p + oplen;
            next_token();
            self_ref = make_node(AST_IDENTIFIER);
            op_node = make_node(op_type);
            n = make_node(AST_ASSIGN);
            if (self_ref != NULL && op_node != NULL && n != NULL) {
                strcpy(self_ref->identifier, id);
                op_node->left = self_ref;
                op_node->right = parse_expression();
                strcpy(n->identifier, id);
                n->right = op_node;
            }
            return n;
        }
    }
    {
        /* subscript assignment: a[i] = e parses as expression first, then
           the '=' upgrades it to a store */
        ASTNode* expr = parse_expression();
        if (expr != NULL && expr->type == AST_INDEX &&
            curr_tok.type == TOK_EQUAL) {
            ASTNode* n;
            next_token();
            n = make_node(AST_STORE);
            if (n != NULL) {
                n->left = expr;
                n->right = parse_expression();
            }
            return n;
        }
        if (expr != NULL && expr->type == AST_ATTR &&
            curr_tok.type == TOK_EQUAL) {
            ASTNode* n;
            next_token();
            n = make_node(AST_SETATTR);
            if (n != NULL) {
                n->left = expr;
                n->right = parse_expression();
            }
            return n;
        }
        return expr;
    }
}

/* One or more simple statements on a single line: a=1; b=2; a+b */
ASTNode* parse_stmt_list(void) {
    ASTNode* left = parse_simple();
    while (curr_tok.type == TOK_SEMICOLON) {
        ASTNode* n;
        next_token(); /* skip ';' */
        if (curr_tok.type == TOK_EOF || curr_tok.type == TOK_NEWLINE) {
            break;
        }
        n = make_node(AST_SEQ);
        if (n != NULL) {
            n->left = left;
            n->right = parse_simple();
        }
        left = n;
    }
    return left;
}

/* Body of while/if/for: either statements on the same line, or an
   indented block on the following lines. */
ASTNode* parse_suite(void) {
    if (curr_tok.type == TOK_NEWLINE) {
        ASTNode* block = NULL;
        next_token();
        if (curr_tok.type != TOK_INDENT) {
            return NULL; /* empty suite */
        }
        next_token();
        while (curr_tok.type != TOK_DEDENT && curr_tok.type != TOK_EOF) {
            ASTNode* stmt;
            if (curr_tok.type == TOK_NEWLINE) {
                next_token();
                continue;
            }
            stmt = parse_statement();
            if (stmt == NULL) {
                break;
            }
            if (block == NULL) {
                block = stmt;
            } else {
                ASTNode* n = make_node(AST_SEQ);
                if (n != NULL) {
                    n->left = block;
                    n->right = stmt;
                }
                block = n;
            }
        }
        if (curr_tok.type == TOK_DEDENT) {
            next_token();
        }
        return block;
    }
    return parse_stmt_list();
}

/* if/elif/else chain. curr_tok is TOK_IF or TOK_ELIF. */
ASTNode* parse_if(void) {
    ASTNode* n = make_node(AST_IF);
    ASTNode* pair;
    next_token(); /* skip 'if'/'elif' */
    if (n != NULL) {
        n->left = parse_expression();
    }
    if (curr_tok.type == TOK_COLON) {
        next_token();
    }
    pair = make_node(AST_ELSE);
    if (pair != NULL) {
        pair->left = parse_suite();
        /* elif/else may sit on the next line at the same indent level;
           newlines between statements are insignificant, so skipping
           them to peek is safe. */
        while (curr_tok.type == TOK_NEWLINE) {
            next_token();
        }
        if (curr_tok.type == TOK_ELIF) {
            pair->right = parse_if();
        } else if (curr_tok.type == TOK_ELSE) {
            next_token();
            if (curr_tok.type == TOK_COLON) {
                next_token();
            }
            pair->right = parse_suite();
        }
    }
    if (n != NULL) {
        n->right = pair;
    }
    return n;
}

ASTNode* parse_statement(void) {
    if (curr_tok.type == TOK_CLASS) {
        ASTNode* n;
        def_mode++; /* whole class body persists like defs */
        n = make_node(AST_CLASS);
        next_token();
        if (n != NULL && curr_tok.type == TOK_IDENTIFIER) {
            strcpy(n->identifier, curr_tok.text);
            next_token();
        }
        if (curr_tok.type == TOK_COLON) {
            next_token();
        }
        if (n != NULL) {
            n->right = parse_suite();
        }
        def_mode--;
        return n;
    }

    if (curr_tok.type == TOK_IMPORT) {
        ASTNode* n = make_node(AST_IMPORT);
        next_token();
        if (n != NULL && curr_tok.type == TOK_IDENTIFIER) {
            strcpy(n->identifier, curr_tok.text);
            next_token();
        }
        return n;
    }

    if (curr_tok.type == TOK_DEF) {
        ASTNode* n;
        ASTNode* param_tail = NULL;
        def_mode++; /* whole subtree goes to the persistent def arena */
        n = make_node(AST_DEF);
        next_token(); /* skip 'def' */
        if (n != NULL && curr_tok.type == TOK_IDENTIFIER) {
            strcpy(n->identifier, curr_tok.text);
            next_token();
        }
        if (curr_tok.type == TOK_LPAREN) {
            next_token();
            while (curr_tok.type == TOK_IDENTIFIER) {
                ASTNode* param = make_node(AST_PARAM);
                if (param != NULL) {
                    strcpy(param->identifier, curr_tok.text);
                }
                if (param_tail == NULL) {
                    if (n != NULL) n->left = param;
                } else {
                    param_tail->right = param;
                }
                param_tail = param;
                next_token();
                if (curr_tok.type == TOK_COMMA) {
                    next_token();
                } else {
                    break;
                }
            }
            if (curr_tok.type == TOK_RPAREN) {
                next_token();
            }
        }
        if (curr_tok.type == TOK_COLON) {
            next_token();
        }
        if (n != NULL) {
            n->right = parse_suite();
        }
        def_mode--;
        return n;
    }

    if (curr_tok.type == TOK_WHILE) {
        ASTNode* n = make_node(AST_WHILE);
        next_token(); /* skip 'while' */
        if (n != NULL) {
            n->left = parse_expression();
            if (curr_tok.type == TOK_COLON) {
                next_token();
            }
            n->right = parse_suite();
        }
        return n;
    }

    if (curr_tok.type == TOK_IF) {
        return parse_if();
    }

    if (curr_tok.type == TOK_FOR) {
        ASTNode* n = make_node(AST_FOR);
        ASTNode* r1 = NULL;
        next_token(); /* skip 'for' */
        if (n != NULL && curr_tok.type == TOK_IDENTIFIER) {
            strcpy(n->identifier, curr_tok.text);
        }
        next_token(); /* skip loop variable */
        if (curr_tok.type == TOK_IN) {
            next_token();
        }
        if (curr_tok.type == TOK_RANGE) {
            next_token();
            if (curr_tok.type == TOK_LPAREN) {
                next_token();
                r1 = make_node(AST_RANGE);
                if (r1 != NULL) {
                    r1->left = parse_expression();
                    if (curr_tok.type == TOK_COMMA) {
                        ASTNode* r2;
                        next_token();
                        r2 = make_node(AST_RANGE);
                        if (r2 != NULL) {
                            r2->left = parse_expression();
                            if (curr_tok.type == TOK_COMMA) {
                                next_token();
                                r2->right = parse_expression();
                            }
                        }
                        r1->right = r2;
                    }
                }
                if (curr_tok.type == TOK_RPAREN) {
                    next_token();
                }
            }
        } else {
            /* for x in <list-or-string expression> */
            if (n != NULL) n->type = AST_FORIN;
            r1 = parse_expression();
        }
        if (curr_tok.type == TOK_COLON) {
            next_token();
        }
        if (n != NULL) {
            n->left = r1;
            n->right = parse_suite();
        }
        return n;
    }

    return parse_stmt_list();
}

static ASTNode* parse_program_body(void);

ASTNode* parse_program(void) BANKED {
    return parse_program_body();
}

ASTNode* parse_module(const char* src) BANKED {
    lexer_reset(src);
    next_token();
    return parse_program_body();
}

/* ROM-baked standard library: modules are gbpython source compiled at
   import time. Module-level mutable state uses the one-element-list
   trick since there is no 'global' keyword. */
static const char module_math[] =
    "pi=3.14159\n"
    "e=2.71828\n"
    "def sqrt(x):\n"
    "    g=float(x)\n"
    "    if g==0.0: return 0.0\n"
    "    for i in range(14): g=(g+x/g)/2\n"
    "    return g\n"
    "def floor(x): return int(x//1)\n"
    "def ceil(x): return 0-int((0-x)//1)\n"
    "def gcd(a,b):\n"
    "    while b: a,b=b,a%b\n"
    "    return a\n";

static const char module_random[] =
    "_rs=[12345]\n"
    "def seed(n):\n"
    "    _rs[0]=n\n"
    "def randint(a,b):\n"
    "    _rs[0]=(_rs[0]*75+74)%65537\n"
    "    return a+_rs[0]%(b-a+1)\n";

ASTNode* import_module(const char* name) BANKED {
    char local[16];
    uint8_t i;
    /* the name lives in banked SRAM; copy it out before comparing */
    SWITCH_RAM(1);
    for (i = 0; i < 15 && name[i]; i++) local[i] = name[i];
    local[i] = '\0';
    if (strcmp(local, "math") == 0) return parse_module(module_math);
    if (strcmp(local, "random") == 0) return parse_module(module_random);
    return NULL;
}

static ASTNode* parse_program_body(void) {
    ASTNode* head = NULL;
    ASTNode* tail = NULL;

    while (curr_tok.type != TOK_EOF) {
        ASTNode* stmt;
        if (curr_tok.type == TOK_NEWLINE || curr_tok.type == TOK_INDENT ||
            curr_tok.type == TOK_DEDENT || curr_tok.type == TOK_SEMICOLON) {
            next_token();
            continue;
        }

        stmt = parse_statement();
        if (stmt) {
            ASTNode* seq = make_node(AST_SEQ);
            if (seq != NULL) {
                seq->left = stmt;
                if (!head) {
                    head = seq;
                    tail = seq;
                } else {
                    tail->right = seq;
                    tail = seq;
                }
            }
        } else {
            break;
        }
    }
    return head;
}

