#pragma once
#include "arena.h"
#include <stddef.h>

typedef enum {
    TOK_ECHO,       /* echo  */
    TOK_FOR,        /* for   */
    TOK_TIME,       /* time  */
    TOK_IN,         /* in    */
    TOK_IDENT,      /* generic identifier / word  */
    TOK_NUMBER,     /* integer literal            */
    TOK_COLON,      /* :     */
    TOK_INDENT,     /* indentation increase       */
    TOK_DEDENT,     /* indentation decrease       */
    TOK_NEWLINE,
    TOK_EOF,
} TokenKind;

typedef struct Token {
    TokenKind   kind;
    const char *val;
    size_t      len;
    int         line;
} Token;

typedef struct TokenList {
    Token  *tokens;
    size_t  count;
    size_t  cap;
} TokenList;

TokenList lex(Arena *arena, const char *src, size_t src_len);
