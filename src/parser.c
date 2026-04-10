#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct NodeList { Node *nodes; size_t count, cap; } NodeList;

static void nl_push(Arena *arena, NodeList *nl, Node node) {
    (void)arena;
    if (nl->count == nl->cap) {
        size_t nc = nl->cap ? nl->cap * 2 : 16;
        nl->nodes = realloc(nl->nodes, nc * sizeof(Node));
        nl->cap   = nc;
    }
    nl->nodes[nl->count++] = node;
}

typedef struct StrList { const char **items; int count, cap; } StrList;

static void sl_push(Arena *arena, StrList *sl, const char *s) {
    (void)arena;
    if (sl->count == sl->cap) {
        int nc = sl->cap ? sl->cap * 2 : 8;
        sl->items = realloc(sl->items, nc * sizeof(char *));
        sl->cap   = nc;
    }
    sl->items[sl->count++] = s;
}

typedef struct Parser { const TokenList *tl; size_t pos; Arena *arena; } Parser;

static Token *peek(Parser *p)    { return &p->tl->tokens[p->pos]; }
static Token *advance(Parser *p) {
    Token *t = &p->tl->tokens[p->pos];
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}
static void skip_newlines(Parser *p) {
    while (peek(p)->kind == TOK_NEWLINE || peek(p)->kind == TOK_SEMI) advance(p);
}

static NodeList parse_block(Parser *p);

/* Collect args until end-of-line (used by echo and cmd) */
static void collect_args(Parser *p, StrList *sl) {
    while (peek(p)->kind != TOK_NEWLINE &&
           peek(p)->kind != TOK_DEDENT  &&
           peek(p)->kind != TOK_EOF     &&
           peek(p)->kind != TOK_SEMI) {
        Token *t = advance(p);
        const char *v = t->val ? t->val : "";
        /* For VAR_REF, prefix with $ to distinguish in codegen */
        if (t->kind == TOK_VAR_REF) {
            char buf[256]; snprintf(buf, sizeof(buf), "$%s", v);
            sl_push(p->arena, sl, arena_strdup(p->arena, buf, strlen(buf)));
        } else {
            sl_push(p->arena, sl, v);
        }
    }
}

/* Parse condition inside [ ... ] */
static Cond parse_cond(Parser *p) {
    Cond c = {0};
    /* skip [ */
    if (peek(p)->kind == TOK_LBRACKET) advance(p);

    /* optional ! */
    if (peek(p)->kind == TOK_OP_NOT) { c.negate = 1; advance(p); }

    /* LHS */
    Token *lhs = advance(p);
    if (lhs->kind == TOK_VAR_REF) {
        char buf[256]; snprintf(buf, sizeof(buf), "$%s", lhs->val);
        c.lhs = arena_strdup(p->arena, buf, strlen(buf));
    } else {
        c.lhs = lhs->val ? lhs->val : "";
    }

    /* operator (optional — for boolean $var) */
    Token *op = peek(p);
    if (op->kind == TOK_OP_EQ  || op->kind == TOK_OP_NEQ ||
        op->kind == TOK_OP_LT  || op->kind == TOK_OP_GT  ||
        op->kind == TOK_OP_LE  || op->kind == TOK_OP_GE  ||
        op->kind == TOK_OP_STR_EQ || op->kind == TOK_ASSIGN) {
        c.op = op->kind;
        advance(p);
        /* RHS */
        Token *rhs = advance(p);
        if (rhs->kind == TOK_VAR_REF) {
            char buf[256]; snprintf(buf, sizeof(buf), "$%s", rhs->val);
            c.rhs = arena_strdup(p->arena, buf, strlen(buf));
        } else {
            c.rhs = rhs->val ? rhs->val : "";
        }
    }

    /* skip ] */
    if (peek(p)->kind == TOK_RBRACKET) advance(p);
    return c;
}

/* Parse indented body after colon */
static NodeList parse_indented_body(Parser *p) {
    skip_newlines(p);
    NodeList body = {0};
    if (peek(p)->kind == TOK_INDENT) {
        advance(p);
        body = parse_block(p);
        if (peek(p)->kind == TOK_DEDENT) advance(p);
    }
    return body;
}

static Node parse_echo(Parser *p, int line) {
    StrList sl = {0};
    collect_args(p, &sl);
    Node node = {0};
    node.kind = NODE_ECHO;
    node.line = line;
    node.args = sl.items;
    node.argc = sl.count;
    return node;
}

static Node parse_cmd(Parser *p, int line, const char *cmd) {
    StrList sl = {0};
    sl_push(p->arena, &sl, cmd);
    collect_args(p, &sl);
    Node node = {0};
    node.kind = NODE_CMD;
    node.line = line;
    node.args = sl.items;
    node.argc = sl.count;
    return node;
}

static Node parse_if(Parser *p, int line) {
    Cond cond = parse_cond(p);
    if (peek(p)->kind == TOK_COLON) advance(p);
    NodeList then_body = parse_indented_body(p);

    Node node = {0};
    node.kind       = NODE_IF;
    node.line       = line;
    node.cond       = cond;
    node.then_body  = then_body.nodes;
    node.then_count = then_body.count;

    /* check for elif / else */
    skip_newlines(p);
    if (peek(p)->kind == TOK_ELSE || peek(p)->kind == TOK_ELIF) {
        int is_elif = (peek(p)->kind == TOK_ELIF);
        advance(p);
        if (is_elif) {
            /* parse as nested if */
            int eline = peek(p)->line;
            Node elif_node = parse_if(p, eline);
            Node *elif_ptr = arena_alloc(p->arena, sizeof(Node));
            *elif_ptr = elif_node;
            node.else_body  = elif_ptr;
            node.else_count = 1;
        } else {
            if (peek(p)->kind == TOK_COLON) advance(p);
            NodeList else_body = parse_indented_body(p);
            node.else_body  = else_body.nodes;
            node.else_count = else_body.count;
        }
    }
    return node;
}

static Node parse_while(Parser *p, int line) {
    Cond cond = parse_cond(p);
    if (peek(p)->kind == TOK_COLON) advance(p);
    NodeList body = parse_indented_body(p);
    Node node = {0};
    node.kind           = NODE_WHILE;
    node.line           = line;
    node.cond           = cond;
    node.then_body      = body.nodes;
    node.then_count     = body.count;
    return node;
}

static Node parse_for(Parser *p, int line) {
    /* for var in list: */
    /* or: for var from to: */
    Token *var_tok = NULL;
    if (peek(p)->kind == TOK_WORD || peek(p)->kind == TOK_VAR_REF)
        var_tok = advance(p);

    Node node = {0};
    node.line = line;

    if (peek(p)->kind == TOK_IN) {
        advance(p); /* consume 'in' */
        StrList list = {0};
        while (peek(p)->kind != TOK_COLON &&
               peek(p)->kind != TOK_NEWLINE &&
               peek(p)->kind != TOK_EOF) {
            Token *t = advance(p);
            if (t->kind == TOK_VAR_REF) {
                char buf[256]; snprintf(buf,sizeof(buf),"$%s",t->val);
                sl_push(p->arena, &list, arena_strdup(p->arena, buf, strlen(buf)));
            } else if (t->val) {
                sl_push(p->arena, &list, t->val);
            }
        }
        if (peek(p)->kind == TOK_COLON) advance(p);
        NodeList body = parse_indented_body(p);
        node.kind            = NODE_FOR_IN;
        node.for_var         = var_tok ? var_tok->val : "i";
        node.for_list        = list.items;
        node.for_list_count  = list.count;
        node.for_body        = body.nodes;
        node.for_body_count  = body.count;
    } else {
        /* numeric: for var from to: */
        int from_val = 0, to_val = 0;
        if (peek(p)->kind == TOK_NUMBER) from_val = atoi(advance(p)->val);
        if (peek(p)->kind == TOK_NUMBER) to_val   = atoi(advance(p)->val);
        if (peek(p)->kind == TOK_COLON)  advance(p);
        NodeList body = parse_indented_body(p);
        node.kind           = NODE_FOR_NUM;
        node.for_var        = var_tok ? var_tok->val : "i";
        node.for_from       = from_val;
        node.for_to         = to_val;
        node.for_body       = body.nodes;
        node.for_body_count = body.count;
    }
    return node;
}

static Node parse_time(Parser *p, int line) {
    if (peek(p)->kind == TOK_COLON) advance(p);
    NodeList body = parse_indented_body(p);
    Node node = {0};
    node.kind           = NODE_TIME;
    node.line           = line;
    node.for_body       = body.nodes;
    node.for_body_count = body.count;
    return node;
}

/* Try to parse var = value assignment. Returns 1 if matched. */
static int try_parse_assign(Parser *p, const char *word, int line, Node *out) {
    size_t saved_pos = p->pos;
    if (peek(p)->kind == TOK_ASSIGN) {
        advance(p); /* consume = */
        Token *val = peek(p);
        const char *raw = "";
        int is_arith = 0;
        if (val->kind == TOK_STRING || val->kind == TOK_WORD ||
            val->kind == TOK_NUMBER || val->kind == TOK_VAR_REF) {
            if (val->kind == TOK_VAR_REF) {
                char buf[256]; snprintf(buf,sizeof(buf),"$%s",val->val);
                raw = arena_strdup(p->arena, buf, strlen(buf));
            } else {
                raw = val->val ? val->val : "";
            }
            advance(p);
        } else if (val->kind == TOK_ARITH) {
            raw = val->val ? val->val : "";
            is_arith = 1;
            advance(p);
        } else {
            /* empty assignment */
        }
        Node node = {0};
        node.kind        = NODE_ASSIGN;
        node.line        = line;
        node.var_name    = word;
        node.var_value   = raw;
        node.var_is_arith= is_arith;
        *out = node;
        return 1;
    }
    p->pos = saved_pos;
    return 0;
}

static NodeList parse_block(Parser *p) {
    NodeList nl = {0};
    while (peek(p)->kind != TOK_DEDENT && peek(p)->kind != TOK_EOF) {
        skip_newlines(p);
        Token *t = peek(p);
        if (t->kind == TOK_DEDENT || t->kind == TOK_EOF) break;

        int ln = t->line;

        if (t->kind == TOK_ECHO) {
            advance(p);
            nl_push(p->arena, &nl, parse_echo(p, ln));
        } else if (t->kind == TOK_FOR) {
            advance(p);
            nl_push(p->arena, &nl, parse_for(p, ln));
        } else if (t->kind == TOK_TIME) {
            advance(p);
            nl_push(p->arena, &nl, parse_time(p, ln));
        } else if (t->kind == TOK_IF) {
            advance(p);
            nl_push(p->arena, &nl, parse_if(p, ln));
        } else if (t->kind == TOK_WHILE) {
            advance(p);
            nl_push(p->arena, &nl, parse_while(p, ln));
        } else if (t->kind == TOK_EXIT) {
            advance(p);
            int code = 0;
            if (peek(p)->kind == TOK_NUMBER) code = atoi(advance(p)->val);
            Node node = {0}; node.kind = NODE_EXIT; node.line = ln; node.exit_code = code;
            nl_push(p->arena, &nl, node);
        } else if (t->kind == TOK_INDENT) {
            advance(p);
            NodeList sub = parse_block(p);
            for (size_t i = 0; i < sub.count; i++)
                nl_push(p->arena, &nl, sub.nodes[i]);
            if (peek(p)->kind == TOK_DEDENT) advance(p);
        } else if (t->kind == TOK_WORD) {
            const char *word = t->val;
            advance(p);
            /* check for assignment */
            Node assign_node = {0};
            if (try_parse_assign(p, word, ln, &assign_node)) {
                nl_push(p->arena, &nl, assign_node);
            } else {
                /* external command */
                nl_push(p->arena, &nl, parse_cmd(p, ln, word));
            }
        } else {
            /* skip unexpected */
            advance(p);
        }
    }
    return nl;
}

AST parse(Arena *arena, const TokenList *tl) {
    Parser p = { tl, 0, arena };
    NodeList nl = parse_block(&p);
    AST ast;
    ast.nodes = nl.nodes;
    ast.count = nl.count;
    ast.cap   = nl.cap;
    return ast;
}
