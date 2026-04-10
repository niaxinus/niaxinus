#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct NodeList { Node *nodes; size_t count, cap; } NodeList;

static void nl_push(Arena *arena, NodeList *nl, Node node) {
    if (nl->count == nl->cap) {
        size_t nc = nl->cap ? nl->cap * 2 : 16;
        Node *buf = arena_alloc(arena, nc * sizeof(Node));
        if (nl->nodes) memcpy(buf, nl->nodes, nl->count * sizeof(Node));
        nl->nodes = buf;
        nl->cap   = nc;
    }
    nl->nodes[nl->count++] = node;
}

typedef struct Parser { const TokenList *tl; size_t pos; Arena *arena; } Parser;

static Token *peek(Parser *p)    { return &p->tl->tokens[p->pos]; }
static Token *advance(Parser *p) {
    Token *t = &p->tl->tokens[p->pos];
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}
static void skip_newlines(Parser *p) {
    while (peek(p)->kind == TOK_NEWLINE) advance(p);
}

static NodeList parse_block(Parser *p);

/* Collect rest-of-line tokens as a single string for echo */
static Node parse_echo(Parser *p, int line) {
    char   buf[4096];
    size_t len = 0;
    int    first = 1;
    while (peek(p)->kind != TOK_NEWLINE &&
           peek(p)->kind != TOK_DEDENT  &&
           peek(p)->kind != TOK_EOF) {
        Token *t = advance(p);
        if (t->val) {
            if (!first && len < sizeof(buf) - 2) buf[len++] = ' ';
            first = 0;
            size_t copy = t->len;
            if (len + copy > sizeof(buf) - 2) copy = sizeof(buf) - 2 - len;
            memcpy(buf + len, t->val, copy);
            len += copy;
        }
    }
    buf[len++] = '\n';
    buf[len]   = '\0';
    Node node = {0};
    node.kind    = NODE_ECHO;
    node.line    = line;
    node.str_val = arena_strdup(p->arena, buf, len);
    node.str_len = len;
    return node;
}

static Node parse_for(Parser *p, int line) {
    Token *var_tok = NULL;
    if (peek(p)->kind == TOK_IDENT || peek(p)->kind == TOK_IN)
        var_tok = advance(p);
    if (peek(p)->kind == TOK_IN) advance(p);
    int from_val = 0, to_val = 0;
    if (peek(p)->kind == TOK_NUMBER) from_val = atoi(advance(p)->val);
    if (peek(p)->kind == TOK_NUMBER) to_val   = atoi(advance(p)->val);
    if (peek(p)->kind == TOK_COLON)  advance(p);
    else fprintf(stderr, "parse: expected ':' after for on line %d\n", line);
    skip_newlines(p);
    NodeList body = {0};
    if (peek(p)->kind == TOK_INDENT) {
        advance(p);
        body = parse_block(p);
        if (peek(p)->kind == TOK_DEDENT) advance(p);
    }
    Node node = {0};
    node.kind           = NODE_FOR;
    node.line           = line;
    node.for_var        = var_tok ? var_tok->val : "i";
    node.for_from       = from_val;
    node.for_to         = to_val;
    node.for_body       = body.nodes;
    node.for_body_count = body.count;
    return node;
}

static Node parse_time(Parser *p, int line) {
    if (peek(p)->kind == TOK_COLON)  advance(p);
    else fprintf(stderr, "parse: expected ':' after time on line %d\n", line);
    skip_newlines(p);
    NodeList body = {0};
    if (peek(p)->kind == TOK_INDENT) {
        advance(p);
        body = parse_block(p);
        if (peek(p)->kind == TOK_DEDENT) advance(p);
    }
    Node node = {0};
    node.kind           = NODE_TIME;
    node.line           = line;
    node.for_body       = body.nodes;
    node.for_body_count = body.count;
    return node;
}

static NodeList parse_block(Parser *p) {
    NodeList nl = {0};
    while (peek(p)->kind != TOK_DEDENT && peek(p)->kind != TOK_EOF) {
        skip_newlines(p);
        Token *t = peek(p);
        if (t->kind == TOK_DEDENT || t->kind == TOK_EOF) break;
        if (t->kind == TOK_ECHO) {
            int ln = t->line; advance(p);
            nl_push(p->arena, &nl, parse_echo(p, ln));
        } else if (t->kind == TOK_FOR) {
            int ln = t->line; advance(p);
            nl_push(p->arena, &nl, parse_for(p, ln));
        } else if (t->kind == TOK_TIME) {
            int ln = t->line; advance(p);
            nl_push(p->arena, &nl, parse_time(p, ln));
        } else if (t->kind == TOK_INDENT) {
            advance(p);
            NodeList sub = parse_block(p);
            for (size_t i = 0; i < sub.count; i++)
                nl_push(p->arena, &nl, sub.nodes[i]);
            if (peek(p)->kind == TOK_DEDENT) advance(p);
        } else {
            fprintf(stderr, "parse: unexpected token '%s' (kind %d) on line %d\n",
                    t->val ? t->val : "(null)", (int)t->kind, t->line);
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
