#include "lexer.h"
#include <string.h>
#include <ctype.h>

static void tl_push(Arena *arena, TokenList *tl, Token tok) {
    if (tl->count == tl->cap) {
        size_t new_cap = tl->cap ? tl->cap * 2 : 128;
        Token *buf = arena_alloc(arena, new_cap * sizeof(Token));
        if (tl->tokens)
            memcpy(buf, tl->tokens, tl->count * sizeof(Token));
        tl->tokens = buf;
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

TokenList lex(Arena *arena, const char *src, size_t src_len) {
    TokenList tl = {0};

    int indent_stack[64];
    int indent_top   = 0;
    indent_stack[0]  = 0;

    size_t i    = 0;
    int    line = 1;
    int    bol  = 1;

    while (i < src_len) {

        if (bol) {
            bol = 0;

            int new_indent = measure_indent(src, i, src_len);
            while (i < src_len && (src[i] == ' ' || src[i] == '\t'))
                i++;

            /* blank line or comment */
            if (i >= src_len || src[i] == '\n' || src[i] == '#') {
                while (i < src_len && src[i] != '\n') i++;
                if (i < src_len) { i++; line++; }
                bol = 1;
                continue;
            }

            if (new_indent > indent_stack[indent_top]) {
                if (indent_top < 63)
                    indent_stack[++indent_top] = new_indent;
                Token t = { TOK_INDENT, NULL, 0, line };
                tl_push(arena, &tl, t);
            } else {
                while (new_indent < indent_stack[indent_top]) {
                    indent_top--;
                    Token t = { TOK_DEDENT, NULL, 0, line };
                    tl_push(arena, &tl, t);
                }
            }
        }

        if (i >= src_len) break;

        char c = src[i];

        if (c == '\n') {
            Token t = { TOK_NEWLINE, NULL, 0, line };
            tl_push(arena, &tl, t);
            line++;
            i++;
            bol = 1;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        if (c == '#') {
            while (i < src_len && src[i] != '\n') i++;
            continue;
        }

        if (c == ':') {
            Token t = { TOK_COLON, ":", 1, line };
            tl_push(arena, &tl, t);
            i++;
            continue;
        }

        if (isdigit((unsigned char)c) ||
            (c == '-' && i + 1 < src_len && isdigit((unsigned char)src[i+1]))) {
            size_t start = i;
            if (src[i] == '-') i++;
            while (i < src_len && isdigit((unsigned char)src[i])) i++;
            Token t = { TOK_NUMBER,
                        arena_strdup(arena, src + start, i - start),
                        i - start, line };
            tl_push(arena, &tl, t);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < src_len &&
                   (isalnum((unsigned char)src[i]) || src[i] == '_' || src[i] == '-'))
                i++;
            size_t len = i - start;
            TokenKind kind = TOK_IDENT;
            if      (len == 4 && memcmp(src+start, "echo", 4) == 0) kind = TOK_ECHO;
            else if (len == 3 && memcmp(src+start, "for",  3) == 0) kind = TOK_FOR;
            else if (len == 4 && memcmp(src+start, "time", 4) == 0) kind = TOK_TIME;
            else if (len == 2 && memcmp(src+start, "in",   2) == 0) kind = TOK_IN;
            Token t = { kind, arena_strdup(arena, src+start, len), len, line };
            tl_push(arena, &tl, t);
            continue;
        }

        {
            Token t = { TOK_IDENT, arena_strdup(arena, src+i, 1), 1, line };
            tl_push(arena, &tl, t);
            i++;
        }
    }

    while (indent_top > 0) {
        indent_top--;
        Token t = { TOK_DEDENT, NULL, 0, line };
        tl_push(arena, &tl, t);
    }

    Token eof = { TOK_EOF, NULL, 0, line };
    tl_push(arena, &tl, eof);
    return tl;
}
