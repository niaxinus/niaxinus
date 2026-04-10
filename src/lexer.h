#pragma once
#include "arena.h"
#include <stddef.h>

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,
    /* literals */
    TOK_WORD,       /* bare word: command name, argument */
    TOK_STRING,     /* "..." or '...' raw content        */
    TOK_NUMBER,     /* integer literal                   */
    TOK_VAR_REF,    /* $varname — stores the name        */
    TOK_ARITH,      /* $(( expr )) — raw expr content    */
    /* punctuation */
    TOK_ASSIGN,     /* =  */
    TOK_COLON,      /* :  */
    TOK_LBRACKET,   /* [  */
    TOK_RBRACKET,   /* ]  */
    TOK_SEMI,       /* ;  */
    TOK_PIPE,       /* |  */
    TOK_BACKGROUND, /* &  */
    TOK_LPAREN,     /* (  */
    TOK_RPAREN,     /* )  */
    TOK_LBRACE,     /* {  */
    TOK_RBRACE,     /* }  */
    TOK_REDIR_IN,   /* <  */
    TOK_REDIR_OUT,  /* >  */
    TOK_REDIR_APPEND, /* >> */
    /* keywords */
    TOK_IF,
    TOK_ELSE,
    TOK_ELIF,
    TOK_FOR,
    TOK_IN,
    TOK_WHILE,
    TOK_ECHO,
    TOK_TIME,
    TOK_EXIT,
    /* comparison operators (inside [ ]) */
    TOK_OP_EQ,      /* == or -eq  */
    TOK_OP_NEQ,     /* != or -ne  */
    TOK_OP_LT,      /* -lt or <   */
    TOK_OP_GT,      /* -gt or >   */
    TOK_OP_LE,      /* -le or <=  */
    TOK_OP_GE,      /* -ge or >=  */
    TOK_OP_STR_EQ,  /* = (string eq in [ ]) */
    TOK_OP_NOT,     /* !          */
    TOK_OP_AND,     /* &&         */
    TOK_OP_OR,      /* ||         */
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
