#define _POSIX_C_SOURCE 200809L
#include "interp.h"

#include "lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_VARS 256

typedef struct {
    char name[64];
    char value[4096];
} InterpVar;

typedef struct {
    InterpVar vars[MAX_VARS];
    int var_count;
    int should_exit;
    int exit_code;
    int last_status;
} InterpState;

static InterpVar *find_var(InterpState *st, const char *name) {
    for (int i = 0; i < st->var_count; i++) {
        if (strcmp(st->vars[i].name, name) == 0) return &st->vars[i];
    }
    return NULL;
}

static const char *get_var(InterpState *st, const char *name) {
    InterpVar *var = find_var(st, name);
    return var ? var->value : "";
}

static void set_var(InterpState *st, const char *name, const char *value) {
    InterpVar *var = find_var(st, name);
    if (!var) {
        if (st->var_count >= MAX_VARS) return;
        var = &st->vars[st->var_count++];
        snprintf(var->name, sizeof(var->name), "%s", name);
        var->value[0] = '\0';
    }
    snprintf(var->value, sizeof(var->value), "%s", value ? value : "");
}

static void eval_string(InterpState *st, const char *raw, char *out, size_t out_sz) {
    size_t oi = 0;
    if (!raw) {
        if (out_sz) out[0] = '\0';
        return;
    }
    for (size_t i = 0; raw[i] && oi + 1 < out_sz; ) {
        if (raw[i] == '$' && (isalpha((unsigned char)raw[i + 1]) || raw[i + 1] == '_')) {
            char name[64];
            int ni = 0;
            i++;
            while (raw[i] && (isalnum((unsigned char)raw[i]) || raw[i] == '_') && ni < 63) {
                name[ni++] = raw[i++];
            }
            name[ni] = '\0';
            const char *val = get_var(st, name);
            for (size_t j = 0; val[j] && oi + 1 < out_sz; j++) out[oi++] = val[j];
        } else {
            out[oi++] = raw[i++];
        }
    }
    out[oi] = '\0';
}

typedef struct {
    const char *s;
    InterpState *st;
} ExprParser;

static void expr_skip_ws(ExprParser *p) {
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n') p->s++;
}

static long parse_expr(ExprParser *p);

static long parse_primary(ExprParser *p) {
    expr_skip_ws(p);
    if (*p->s == '(') {
        p->s++;
        long value = parse_expr(p);
        expr_skip_ws(p);
        if (*p->s == ')') p->s++;
        return value;
    }

    if (*p->s == '$') p->s++;
    if (isalpha((unsigned char)*p->s) || *p->s == '_') {
        char name[64];
        int ni = 0;
        while ((isalnum((unsigned char)*p->s) || *p->s == '_') && ni < 63) {
            name[ni++] = *p->s++;
        }
        name[ni] = '\0';
        return atol(get_var(p->st, name));
    }

    char *end = NULL;
    long value = strtol(p->s, &end, 10);
    if (end != p->s) {
        p->s = end;
        return value;
    }
    return 0;
}

static long parse_unary(ExprParser *p) {
    expr_skip_ws(p);
    if (*p->s == '+') {
        p->s++;
        return parse_unary(p);
    }
    if (*p->s == '-') {
        p->s++;
        return -parse_unary(p);
    }
    return parse_primary(p);
}

static long ipow(long base, long exp) {
    long result = 1;
    while (exp > 0) {
        if (exp & 1L) result *= base;
        base *= base;
        exp >>= 1L;
    }
    return result;
}

static long parse_power(ExprParser *p) {
    long lhs = parse_unary(p);
    expr_skip_ws(p);
    if (p->s[0] == '*' && p->s[1] == '*') {
        p->s += 2;
        return ipow(lhs, parse_power(p));
    }
    return lhs;
}

static long parse_mul(ExprParser *p) {
    long lhs = parse_power(p);
    for (;;) {
        expr_skip_ws(p);
        if (*p->s == '*') {
            p->s++;
            lhs *= parse_power(p);
        } else if (*p->s == '/') {
            p->s++;
            long rhs = parse_power(p);
            lhs = rhs ? lhs / rhs : 0;
        } else if (*p->s == '%') {
            p->s++;
            long rhs = parse_power(p);
            lhs = rhs ? lhs % rhs : 0;
        } else {
            return lhs;
        }
    }
}

static long parse_expr(ExprParser *p) {
    long lhs = parse_mul(p);
    for (;;) {
        expr_skip_ws(p);
        if (*p->s == '+') {
            p->s++;
            lhs += parse_mul(p);
        } else if (*p->s == '-') {
            p->s++;
            lhs -= parse_mul(p);
        } else {
            return lhs;
        }
    }
}

static long eval_arith(InterpState *st, const char *expr) {
    ExprParser p = { expr ? expr : "0", st };
    return parse_expr(&p);
}

static int eval_cond(InterpState *st, const Cond *cond) {
    char lhs[4096];
    char rhs[4096];
    eval_string(st, cond->lhs ? cond->lhs : "", lhs, sizeof(lhs));
    eval_string(st, cond->rhs ? cond->rhs : "", rhs, sizeof(rhs));

    int result = 0;
    switch (cond->op) {
        case 0: result = lhs[0] != '\0'; break;
        case TOK_OP_STR_EQ:
        case TOK_ASSIGN: result = strcmp(lhs, rhs) == 0; break;
        case TOK_OP_EQ: result = atol(lhs) == atol(rhs); break;
        case TOK_OP_NEQ: result = atol(lhs) != atol(rhs); break;
        case TOK_OP_LT: result = atol(lhs) < atol(rhs); break;
        case TOK_OP_GT: result = atol(lhs) > atol(rhs); break;
        case TOK_OP_LE: result = atol(lhs) <= atol(rhs); break;
        case TOK_OP_GE: result = atol(lhs) >= atol(rhs); break;
        default: result = strcmp(lhs, rhs) == 0; break;
    }
    return cond->negate ? !result : result;
}

static int exec_nodes(InterpState *st, const Node *nodes, size_t count);

static int exec_cmd(InterpState *st, const Node *n) {
    char *argv[256];
    if (n->argc <= 0) return 0;
    for (int i = 0; i < n->argc && i < 255; i++) {
        char buf[4096];
        eval_string(st, n->args[i], buf, sizeof(buf));
        argv[i] = strdup(buf);
    }
    argv[n->argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    int status = 1;
    if (pid > 0) {
        int raw = 0;
        waitpid(pid, &raw, 0);
        if (WIFEXITED(raw)) status = WEXITSTATUS(raw);
    }

    for (int i = 0; i < n->argc; i++) free(argv[i]);
    st->last_status = status;
    return status;
}

static int exec_node(InterpState *st, const Node *n) {
    switch (n->kind) {
        case NODE_ECHO: {
            for (int i = 0; i < n->argc; i++) {
                char buf[4096];
                eval_string(st, n->args[i], buf, sizeof(buf));
                if (i) putchar(' ');
                fputs(buf, stdout);
            }
            putchar('\n');
            st->last_status = 0;
            return 0;
        }
        case NODE_ASSIGN: {
            char buf[4096];
            if (n->var_is_arith) {
                snprintf(buf, sizeof(buf), "%ld", eval_arith(st, n->var_value));
            } else {
                eval_string(st, n->var_value, buf, sizeof(buf));
            }
            set_var(st, n->var_name, buf);
            st->last_status = 0;
            return 0;
        }
        case NODE_IF:
            if (eval_cond(st, &n->cond)) return exec_nodes(st, n->then_body, n->then_count);
            return exec_nodes(st, n->else_body, n->else_count);
        case NODE_WHILE:
            while (!st->should_exit && eval_cond(st, &n->cond)) {
                int rc = exec_nodes(st, n->then_body, n->then_count);
                if (rc && st->should_exit) return rc;
            }
            return st->last_status;
        case NODE_FOR_IN:
            for (int i = 0; i < n->for_list_count && !st->should_exit; i++) {
                char buf[4096];
                eval_string(st, n->for_list[i], buf, sizeof(buf));
                set_var(st, n->for_var, buf);
                exec_nodes(st, n->for_body, n->for_body_count);
            }
            return st->last_status;
        case NODE_FOR_NUM:
            for (long i = n->for_from; i <= n->for_to && !st->should_exit; i++) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%ld", i);
                set_var(st, n->for_var, buf);
                exec_nodes(st, n->for_body, n->for_body_count);
            }
            return st->last_status;
        case NODE_TIME: {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            exec_nodes(st, n->for_body, n->for_body_count);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
            fprintf(stderr, "time: %ld ns\n", ns);
            return st->last_status;
        }
        case NODE_CMD:
            return exec_cmd(st, n);
        case NODE_EXIT:
            st->should_exit = 1;
            st->exit_code = n->exit_code;
            st->last_status = n->exit_code;
            return n->exit_code;
        case NODE_BLOCK:
            return exec_nodes(st, n->for_body, n->for_body_count);
    }
    return 0;
}

static int exec_nodes(InterpState *st, const Node *nodes, size_t count) {
    for (size_t i = 0; i < count && !st->should_exit; i++) {
        exec_node(st, &nodes[i]);
    }
    return st->last_status;
}

int interp_run(const AST *ast) {
    InterpState st = {0};
    exec_nodes(&st, ast->nodes, ast->count);
    return st.should_exit ? st.exit_code : st.last_status;
}

