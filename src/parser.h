#pragma once
#include "arena.h"
#include "lexer.h"
#include <stdint.h>

typedef enum {
    NODE_ECHO,
    NODE_FOR,
    NODE_TIME,
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind    kind;
    int         line;

    /* NODE_ECHO */
    const char *str_val;
    size_t      str_len;
    uint64_t    str_va;      /* filled by codegen pre-pass */

    /* NODE_FOR / NODE_TIME body */
    const char *for_var;
    int         for_from;
    int         for_to;
    Node       *for_body;
    size_t      for_body_count;
};

typedef struct AST {
    Node   *nodes;
    size_t  count;
    size_t  cap;
} AST;

AST parse(Arena *arena, const TokenList *tl);
