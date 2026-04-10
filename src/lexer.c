#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static void tl_push(Arena *arena, TokenList *tl, Token tok) {
    (void)arena;
    if (tl->count == tl->cap) {
        size_t new_cap = tl->cap ? tl->cap * 2 : 128;
        tl->tokens = realloc(tl->tokens, new_cap * sizeof(Token));
        tl->cap    = new_cap;
    }
    tl->tokens[tl->count++] = tok;
}

static int measure_indent(const char *src, size_t i, size_t src_len) {
    int indent = 0;
    while (i < src_len && (src[i] == ' ' || src[i] == '\t')) {
        indent += (src[i] == '\t') ? 4 : 1;
        i++;
    }
    return indent;
}

/* Push a simple token */
#define PUSH(k, v, l) do { Token _t = {(k),(v),(l),line}; tl_push(arena,&tl,_t); } while(0)

TokenList lex(Arena *arena, const char *src, size_t src_len) {
    TokenList tl = {0};

    int indent_stack[64];
    int indent_top  = 0;
    indent_stack[0] = 0;

    size_t i    = 0;
    int    line = 1;
    int    bol  = 1;

    while (i < src_len) {

        /* ---- beginning-of-line: handle indentation ---- */
        if (bol) {
            bol = 0;
            int new_indent = measure_indent(src, i, src_len);
            while (i < src_len && (src[i] == ' ' || src[i] == '\t')) i++;

            /* blank line or comment — skip */
            if (i >= src_len || src[i] == '\n' || src[i] == '#') {
                while (i < src_len && src[i] != '\n') i++;
                if (i < src_len) { i++; line++; }
                bol = 1;
                continue;
            }

            if (new_indent > indent_stack[indent_top]) {
                if (indent_top < 63) indent_stack[++indent_top] = new_indent;
                PUSH(TOK_INDENT, NULL, 0);
            } else {
                while (new_indent < indent_stack[indent_top]) {
                    indent_top--;
                    PUSH(TOK_DEDENT, NULL, 0);
                }
            }
        }

        if (i >= src_len) break;
        char c = src[i];

        /* newline */
        if (c == '\n') {
            PUSH(TOK_NEWLINE, NULL, 0);
            line++; i++; bol = 1;
            continue;
        }

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        /* comment */
        if (c == '#') {
            while (i < src_len && src[i] != '\n') i++;
            continue;
        }

        /* colon */
        if (c == ':') { PUSH(TOK_COLON, ":", 1); i++; continue; }

        /* semicolon */
        if (c == ';') { PUSH(TOK_SEMI, ";", 1); i++; continue; }

        /* brackets */
        if (c == '[') { PUSH(TOK_LBRACKET, "[", 1); i++; continue; }
        if (c == ']') { PUSH(TOK_RBRACKET, "]", 1); i++; continue; }

        /* && || */
        if (c == '&' && i+1 < src_len && src[i+1] == '&') {
            PUSH(TOK_OP_AND, "&&", 2); i += 2; continue;
        }
        if (c == '|' && i+1 < src_len && src[i+1] == '|') {
            PUSH(TOK_OP_OR, "||", 2); i += 2; continue;
        }

        /* != */
        if (c == '!' && i+1 < src_len && src[i+1] == '=') {
            PUSH(TOK_OP_NEQ, "!=", 2); i += 2; continue;
        }
        /* ! */
        if (c == '!') { PUSH(TOK_OP_NOT, "!", 1); i++; continue; }

        /* == or = */
        if (c == '=') {
            if (i+1 < src_len && src[i+1] == '=') {
                PUSH(TOK_OP_EQ, "==", 2); i += 2;
            } else {
                PUSH(TOK_ASSIGN, "=", 1); i++;
            }
            continue;
        }

        /* <= or < */
        if (c == '<') {
            if (i+1 < src_len && src[i+1] == '=') {
                PUSH(TOK_OP_LE, "<=", 2); i += 2;
            } else {
                PUSH(TOK_OP_LT, "<", 1); i++;
            }
            continue;
        }

        /* >= or > */
        if (c == '>') {
            if (i+1 < src_len && src[i+1] == '=') {
                PUSH(TOK_OP_GE, ">=", 2); i += 2;
            } else {
                PUSH(TOK_OP_GT, ">", 1); i++;
            }
            continue;
        }

        /* $((expr)) or $var */
        if (c == '$') {
            i++;
            if (i+1 < src_len && src[i] == '(' && src[i+1] == '(') {
                /* arithmetic expression $(( ... )) */
                i += 2;
                size_t start = i;
                int depth = 2;
                while (i < src_len && depth > 0) {
                    if (src[i] == '(') depth++;
                    else if (src[i] == ')') depth--;
                    if (depth > 0) i++;
                    else i++;
                }
                /* skip trailing ) if any */
                size_t len = (i - 2) - start;
                if ((int)len < 0) len = 0;
                const char *expr = arena_strdup(arena, src + start, len);
                PUSH(TOK_ARITH, expr, len);
            } else if (isalpha((unsigned char)src[i]) || src[i] == '_') {
                size_t start = i;
                while (i < src_len && (isalnum((unsigned char)src[i]) || src[i]=='_')) i++;
                const char *name = arena_strdup(arena, src+start, i-start);
                PUSH(TOK_VAR_REF, name, i-start);
            } else {
                /* bare $, treat as word */
                PUSH(TOK_WORD, "$", 1);
            }
            continue;
        }

        /* quoted string */
        if (c == '"' || c == '\'') {
            char quote = c; i++;
            size_t start = i;
            /* collect raw content (we keep $var intact for codegen) */
            char buf[4096]; size_t blen = 0;
            while (i < src_len && src[i] != quote) {
                if (src[i] == '\\' && quote == '"' && i+1 < src_len) {
                    i++;
                    char esc = src[i++];
                    switch (esc) {
                        case 'n': if (blen<4095) buf[blen++]='\n'; break;
                        case 't': if (blen<4095) buf[blen++]='\t'; break;
                        case '"': if (blen<4095) buf[blen++]='"'; break;
                        case '\\': if (blen<4095) buf[blen++]='\\'; break;
                        default:
                            if (blen<4095) buf[blen++]='\\';
                            if (blen<4095) buf[blen++]=esc;
                            break;
                    }
                } else {
                    if (blen < 4095) buf[blen++] = src[i];
                    i++;
                }
            }
            if (i < src_len) i++; /* closing quote */
            buf[blen] = '\0';
            (void)start;
            const char *s = arena_strdup(arena, buf, blen);
            PUSH(TOK_STRING, s, blen);
            continue;
        }

        /* number (bare, not negative — negative handled as word with minus) */
        if (isdigit((unsigned char)c)) {
            size_t start = i;
            while (i < src_len && isdigit((unsigned char)src[i])) i++;
            PUSH(TOK_NUMBER, arena_strdup(arena, src+start, i-start), i-start);
            continue;
        }

        /* identifier / keyword / operator word */
        if (isalpha((unsigned char)c) || c == '_' ||
            (c == '-' && i+1 < src_len && isalpha((unsigned char)src[i+1]))) {
            size_t start = i;
            if (c == '-') i++; /* consume leading - for -eq, -lt etc. */
            while (i < src_len &&
                   (isalnum((unsigned char)src[i]) || src[i]=='_' ||
                    (src[i]=='-' && i > start) ||
                    src[i]=='/' || src[i]=='.'))
                i++;
            size_t len = i - start;
            const char *val = arena_strdup(arena, src+start, len);
            TokenKind kind;
            /* keyword check */
            if      (len==4 && !memcmp(val,"echo",4))   kind = TOK_ECHO;
            else if (len==3 && !memcmp(val,"for",3))    kind = TOK_FOR;
            else if (len==4 && !memcmp(val,"time",4))   kind = TOK_TIME;
            else if (len==2 && !memcmp(val,"in",2))     kind = TOK_IN;
            else if (len==2 && !memcmp(val,"if",2))     kind = TOK_IF;
            else if (len==4 && !memcmp(val,"else",4))   kind = TOK_ELSE;
            else if (len==4 && !memcmp(val,"elif",4))   kind = TOK_ELIF;
            else if (len==5 && !memcmp(val,"while",5))  kind = TOK_WHILE;
            else if (len==4 && !memcmp(val,"exit",4))   kind = TOK_EXIT;
            /* comparison operators */
            else if (len==3 && !memcmp(val,"-eq",3))    kind = TOK_OP_EQ;
            else if (len==3 && !memcmp(val,"-ne",3))    kind = TOK_OP_NEQ;
            else if (len==3 && !memcmp(val,"-lt",3))    kind = TOK_OP_LT;
            else if (len==3 && !memcmp(val,"-gt",3))    kind = TOK_OP_GT;
            else if (len==3 && !memcmp(val,"-le",3))    kind = TOK_OP_LE;
            else if (len==3 && !memcmp(val,"-ge",3))    kind = TOK_OP_GE;
            else                                         kind = TOK_WORD;
            PUSH(kind, val, len);
            continue;
        }

        /* anything else: treat as single-char word */
        {
            char buf2[2] = {c, '\0'};
            PUSH(TOK_WORD, arena_strdup(arena, buf2, 1), 1);
            i++;
        }
    }

    /* emit remaining DEDENTs */
    while (indent_top > 0) {
        indent_top--;
        PUSH(TOK_DEDENT, NULL, 0);
    }
    PUSH(TOK_EOF, NULL, 0);
    return tl;
}
