#pragma once
#include "arena.h"
#include "lexer.h"
#include <stddef.h>

typedef enum {
    NODE_ECHO,        /* echo <args...>              */
    NODE_ASSIGN,      /* var = value                 */
    NODE_IF,          /* if [ cond ]: body else: ... */
    NODE_WHILE,       /* while [ cond ]: body        */
    NODE_FOR_IN,      /* for var in list: body       */
    NODE_FOR_NUM,     /* for var from to: body       */
    NODE_TIME,        /* time: body                  */
    NODE_CMD,         /* external/builtin command    */
    NODE_EXIT,        /* exit [code]                 */
    NODE_BLOCK,       /* sequence of nodes           */
} NodeKind;

/* A single condition like: $x -gt 3  or  "str" == $y */
typedef struct Cond {
    const char *lhs;   /* left operand (raw, may be $var or literal) */
    TokenKind   op;    /* TOK_OP_EQ, TOK_OP_LT, etc.  0 = no op (boolean $var) */
    const char *rhs;   /* right operand, or NULL */
    int         negate; /* ! prefix */
} Cond;

typedef struct Node Node;

struct Node {
    NodeKind    kind;
    int         line;

    /* NODE_ECHO: tokens as space-separated args (raw, may have $var) */
    const char **args;
    int          argc;

    /* NODE_ASSIGN */
    const char *var_name;   /* left-hand side */
    const char *var_value;  /* right-hand side raw string (may have $var) */
    int         var_is_arith; /* 1 if var_value is $(( expr )) */

    /* NODE_IF */
    Cond        cond;
    Node       *then_body;
    size_t      then_count;
    Node       *else_body;
    size_t      else_count;

    /* NODE_WHILE */
    /* uses cond, then_body/then_count */

    /* NODE_FOR_IN */
    const char  *for_var;
    const char **for_list;
    int          for_list_count;

    /* NODE_FOR_NUM (keeps backward compat) */
    int          for_from;
    int          for_to;

    /* body shared by FOR_IN, FOR_NUM, TIME */
    Node        *for_body;
    size_t       for_body_count;

    /* NODE_CMD: args[0] = command, args[1..argc-1] = arguments */

    /* NODE_EXIT */
    int          exit_code;
};

typedef struct AST {
    Node   *nodes;
    size_t  count;
    size_t  cap;
} AST;

AST parse(Arena *arena, const TokenList *tl);
