#include "debug.h"

#include <stdio.h>

static const char *token_name(TokenKind kind) {
    switch (kind) {
        case TOK_EOF: return "EOF";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_INDENT: return "INDENT";
        case TOK_DEDENT: return "DEDENT";
        case TOK_WORD: return "WORD";
        case TOK_STRING: return "STRING";
        case TOK_NUMBER: return "NUMBER";
        case TOK_VAR_REF: return "VAR_REF";
        case TOK_ARITH: return "ARITH";
        case TOK_ASSIGN: return "ASSIGN";
        case TOK_COLON: return "COLON";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_SEMI: return "SEMI";
        case TOK_IF: return "IF";
        case TOK_ELSE: return "ELSE";
        case TOK_ELIF: return "ELIF";
        case TOK_FOR: return "FOR";
        case TOK_IN: return "IN";
        case TOK_WHILE: return "WHILE";
        case TOK_ECHO: return "ECHO";
        case TOK_TIME: return "TIME";
        case TOK_EXIT: return "EXIT";
        case TOK_OP_EQ: return "OP_EQ";
        case TOK_OP_NEQ: return "OP_NEQ";
        case TOK_OP_LT: return "OP_LT";
        case TOK_OP_GT: return "OP_GT";
        case TOK_OP_LE: return "OP_LE";
        case TOK_OP_GE: return "OP_GE";
        case TOK_OP_STR_EQ: return "OP_STR_EQ";
        case TOK_OP_NOT: return "OP_NOT";
        case TOK_OP_AND: return "OP_AND";
        case TOK_OP_OR: return "OP_OR";
    }
    return "UNKNOWN";
}

static const char *node_name(NodeKind kind) {
    switch (kind) {
        case NODE_ECHO: return "ECHO";
        case NODE_ASSIGN: return "ASSIGN";
        case NODE_IF: return "IF";
        case NODE_WHILE: return "WHILE";
        case NODE_FOR_IN: return "FOR_IN";
        case NODE_FOR_NUM: return "FOR_NUM";
        case NODE_TIME: return "TIME";
        case NODE_CMD: return "CMD";
        case NODE_EXIT: return "EXIT";
        case NODE_BLOCK: return "BLOCK";
    }
    return "UNKNOWN";
}

void dump_tokens(FILE *out, const TokenList *tl) {
    for (size_t i = 0; i < tl->count; i++) {
        const Token *t = &tl->tokens[i];
        fprintf(out, "%4d  %-10s", t->line, token_name(t->kind));
        if (t->val) {
            fprintf(out, "  %s", t->val);
        }
        fputc('\n', out);
    }
}

static void indent(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
}

static void dump_cond(FILE *out, const Cond *cond, int depth) {
    indent(out, depth);
    fprintf(out, "cond negate=%d lhs=%s", cond->negate, cond->lhs ? cond->lhs : "");
    if (cond->op) fprintf(out, " op=%s", token_name(cond->op));
    if (cond->rhs) fprintf(out, " rhs=%s", cond->rhs);
    fputc('\n', out);
}

static void dump_nodes(FILE *out, const Node *nodes, size_t count, int depth) {
    for (size_t i = 0; i < count; i++) {
        const Node *n = &nodes[i];
        indent(out, depth);
        fprintf(out, "%s", node_name(n->kind));
        switch (n->kind) {
            case NODE_ASSIGN:
                fprintf(out, " %s = ", n->var_name ? n->var_name : "");
                if (n->var_is_arith) {
                    fprintf(out, "$(( %s ))\n", n->var_value ? n->var_value : "");
                } else {
                    fprintf(out, "%s\n", n->var_value ? n->var_value : "");
                }
                break;
            case NODE_ECHO:
            case NODE_CMD:
                fprintf(out, " argc=%d\n", n->argc);
                for (int j = 0; j < n->argc; j++) {
                    indent(out, depth + 1);
                    fprintf(out, "arg[%d]=%s\n", j, n->args[j]);
                }
                break;
            case NODE_IF:
                fputc('\n', out);
                dump_cond(out, &n->cond, depth + 1);
                indent(out, depth + 1);
                fputs("then\n", out);
                dump_nodes(out, n->then_body, n->then_count, depth + 2);
                if (n->else_count) {
                    indent(out, depth + 1);
                    fputs("else\n", out);
                    dump_nodes(out, n->else_body, n->else_count, depth + 2);
                }
                break;
            case NODE_WHILE:
                fputc('\n', out);
                dump_cond(out, &n->cond, depth + 1);
                dump_nodes(out, n->then_body, n->then_count, depth + 1);
                break;
            case NODE_FOR_IN:
                fprintf(out, " %s in (%d items)\n", n->for_var, n->for_list_count);
                for (int j = 0; j < n->for_list_count; j++) {
                    indent(out, depth + 1);
                    fprintf(out, "item[%d]=%s\n", j, n->for_list[j]);
                }
                dump_nodes(out, n->for_body, n->for_body_count, depth + 1);
                break;
            case NODE_FOR_NUM:
                fprintf(out, " %s %d..%d\n", n->for_var, n->for_from, n->for_to);
                dump_nodes(out, n->for_body, n->for_body_count, depth + 1);
                break;
            case NODE_TIME:
                fputc('\n', out);
                dump_nodes(out, n->for_body, n->for_body_count, depth + 1);
                break;
            case NODE_EXIT:
                fprintf(out, " %d\n", n->exit_code);
                break;
            case NODE_BLOCK:
                fputc('\n', out);
                break;
        }
    }
}

void dump_ast(FILE *out, const AST *ast) {
    dump_nodes(out, ast->nodes, ast->count, 0);
}
