#define _POSIX_C_SOURCE 200809L
#include "codegen.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* =========================================================
   Variable collection pass
   ========================================================= */

#define MAX_VARS 256
static char var_names[MAX_VARS][64];
static int  var_count = 0;

static void add_var(const char *name) {
    if (!name) return;
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_names[i], name) == 0) return;
    if (var_count < MAX_VARS)
        strncpy(var_names[var_count++], name, 63);
}

/* Find $var references in a raw string like "hello $name world" */
static void scan_var_refs(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '$') {
            s++;
            char name[64]; int ni = 0;
            while (*s && (isalnum((unsigned char)*s) || *s=='_') && ni < 63)
                name[ni++] = *s++;
            name[ni] = '\0';
            if (ni > 0) add_var(name);
        } else s++;
    }
}

static void collect_vars_nodes(const Node *nodes, size_t count);

static void collect_vars_node(const Node *n) {
    switch (n->kind) {
        case NODE_ASSIGN:
            add_var(n->var_name);
            scan_var_refs(n->var_value);
            break;
        case NODE_ECHO:
        case NODE_CMD:
            for (int i = 0; i < n->argc; i++) scan_var_refs(n->args[i]);
            break;
        case NODE_IF:
            scan_var_refs(n->cond.lhs);
            scan_var_refs(n->cond.rhs);
            collect_vars_nodes(n->then_body, n->then_count);
            collect_vars_nodes(n->else_body, n->else_count);
            break;
        case NODE_WHILE:
            scan_var_refs(n->cond.lhs);
            scan_var_refs(n->cond.rhs);
            collect_vars_nodes(n->then_body, n->then_count);
            break;
        case NODE_FOR_IN:
            add_var(n->for_var);
            for (int i = 0; i < n->for_list_count; i++) scan_var_refs(n->for_list[i]);
            collect_vars_nodes(n->for_body, n->for_body_count);
            break;
        case NODE_FOR_NUM:
            add_var(n->for_var);
            collect_vars_nodes(n->for_body, n->for_body_count);
            break;
        case NODE_TIME:
            collect_vars_nodes(n->for_body, n->for_body_count);
            break;
        default: break;
    }
}

static void collect_vars_nodes(const Node *nodes, size_t count) {
    for (size_t i = 0; i < count; i++) collect_vars_node(&nodes[i]);
}

/* =========================================================
   C code emission helpers
   ========================================================= */

static void indent_c(FILE *f, int depth) {
    for (int i = 0; i < depth; i++) fprintf(f, "    ");
}

/*
 * Emit a C expression for a raw value that may contain $var references.
 * Returns: writes into a static buffer declaration and uses _nxs_str() macro.
 * For simple cases (no $), just emits the literal.
 *
 * We build it as: _nxs_buf(buf, sizeof(buf)), then strcat pieces.
 * But for inline use we use a helper macro _S(raw) that expands at codegen time.
 */

/* Check if string has any $var reference */
static int has_var_ref(const char *s) {
    if (!s) return 0;
    while (*s) { if (*s == '$') return 1; s++; }
    return 0;
}

/* Emit a string literal for C, escaping special chars */
static void emit_c_str_literal(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:   fputc(*s, f);     break;
        }
    }
    fputc('"', f);
}

/*
 * Emit a C expression that produces a string value from a raw arg.
 * If the arg is "$varname", emit _v_varname.
 * If it's a plain string with $refs, emit _nxs_interp() call.
 * Otherwise emit a string literal.
 * Result is always a char* expression.
 */
static void emit_c_string_expr(FILE *f, const char *raw) {
    if (!raw || !*raw) { fputs("\"\"", f); return; }

    /* Simple $var reference */
    if (raw[0] == '$' && raw[1] && raw[1] != '{') {
        fprintf(f, "_v_%s", raw+1);
        return;
    }

    if (!has_var_ref(raw)) {
        emit_c_str_literal(f, raw);
        return;
    }

    /* Complex: build format string + args for snprintf */
    /* We emit: _nxs_interp(fmt, ...) which is defined as a macro */
    /* Actually we emit an inline block expression: */
    /* ({ static char _b[4096]; _nxs_build(_b, raw_pieces...); _b; }) */
    /* Simpler: just emit _nxs_interp("fmt", args...) */
    fprintf(f, "_nxs_interp(");

    /* Build format string and arg list */
    char fmt[2048]; int fi = 0;
    char args[2048]; int ai = 0;
    const char *s = raw;
    fmt[fi++] = '"';
    while (*s) {
        if (*s == '$') {
            s++;
            char name[64]; int ni = 0;
            while (*s && (isalnum((unsigned char)*s)||*s=='_') && ni<63)
                name[ni++] = *s++;
            name[ni] = '\0';
            fmt[fi++] = '%'; fmt[fi++] = 's';
            int written = snprintf(args+ai, sizeof(args)-ai, ",_v_%s", name);
            if (written > 0) ai += written;
        } else {
            if (*s == '"') { fmt[fi++]='\\'; fmt[fi++]='"'; }
            else if (*s == '\\') { fmt[fi++]='\\'; fmt[fi++]='\\'; }
            else if (*s == '\n') { fmt[fi++]='\\'; fmt[fi++]='n'; }
            else fmt[fi++] = *s;
            s++;
        }
    }
    fmt[fi++] = '"'; fmt[fi] = '\0';
    args[ai] = '\0';
    fprintf(f, "%s%s)", fmt, args);
}

/*
 * Emit a C boolean condition from a Cond struct.
 */
static void emit_c_cond(FILE *f, const Cond *c) {
    if (c->negate) fputs("!(", f);

    int numeric = (c->op == TOK_OP_EQ || c->op == TOK_OP_NEQ ||
                   c->op == TOK_OP_LT || c->op == TOK_OP_GT  ||
                   c->op == TOK_OP_LE || c->op == TOK_OP_GE);

    if (c->op == 0) {
        /* boolean: non-empty string */
        fprintf(f, "(*");
        emit_c_string_expr(f, c->lhs);
        fprintf(f, " != '\\0')");
    } else if (c->op == TOK_OP_STR_EQ || c->op == TOK_ASSIGN) {
        fprintf(f, "strcmp(");
        emit_c_string_expr(f, c->lhs);
        fprintf(f, ",");
        emit_c_string_expr(f, c->rhs ? c->rhs : "");
        fprintf(f, ") == 0");
    } else if (numeric) {
        fprintf(f, "atol(");
        emit_c_string_expr(f, c->lhs);
        fprintf(f, ")");
        switch (c->op) {
            case TOK_OP_EQ:  fputs(" == ", f); break;
            case TOK_OP_NEQ: fputs(" != ", f); break;
            case TOK_OP_LT:  fputs(" < ",  f); break;
            case TOK_OP_GT:  fputs(" > ",  f); break;
            case TOK_OP_LE:  fputs(" <= ", f); break;
            case TOK_OP_GE:  fputs(" >= ", f); break;
            default: fputs(" == ", f); break;
        }
        fprintf(f, "atol(");
        emit_c_string_expr(f, c->rhs ? c->rhs : "0");
        fprintf(f, ")");
    } else {
        /* fallback string equality */
        fprintf(f, "strcmp(");
        emit_c_string_expr(f, c->lhs);
        fprintf(f, ",");
        emit_c_string_expr(f, c->rhs ? c->rhs : "");
        fprintf(f, ") == 0");
    }

    if (c->negate) fputs(")", f);
}

/* Forward decl */
static void emit_c_nodes(FILE *f, const Node *nodes, size_t count, int depth);

static void emit_c_node(FILE *f, const Node *n, int depth) {
    switch (n->kind) {

        case NODE_ECHO: {
            indent_c(f, depth);
            fprintf(f, "_nxs_echo(");
            for (int i = 0; i < n->argc; i++) {
                if (i > 0) fputs(", ", f);
                emit_c_string_expr(f, n->args[i]);
            }
            if (n->argc == 0) fprintf(f, "\"\"");
            fprintf(f, ");\n");
            break;
        }

        case NODE_ASSIGN: {
            indent_c(f, depth);
            if (n->var_is_arith) {
                /* Translate arithmetic expr: bare 'a' → atol(_v_a), '$a' → atol(_v_a) */
                const char *expr = n->var_value ? n->var_value : "0";
                char translated[2048]; int ti = 0;
                const char *p = expr;
                while (*p && ti < 2040) {
                    /* skip $var or bare identifier → atol(_v_name) */
                    int is_dollar = (*p == '$');
                    if (is_dollar) p++;
                    if (isalpha((unsigned char)*p) || *p == '_') {
                        char name[64]; int ni = 0;
                        while (*p && (isalnum((unsigned char)*p)||*p=='_') && ni<63)
                            name[ni++] = *p++;
                        name[ni] = '\0';
                        int written = snprintf(translated+ti, sizeof(translated)-ti,
                                               "atol(_v_%s)", name);
                        if (written > 0) ti += written;
                    } else {
                        if (is_dollar) { translated[ti++] = '$'; }
                        translated[ti++] = *p++;
                    }
                }
                translated[ti] = '\0';
                fprintf(f, "snprintf(_v_%s, sizeof(_v_%s), \"%%ld\", (long)(%s));\n",
                        n->var_name, n->var_name, translated);
            } else {
                fprintf(f, "snprintf(_v_%s, sizeof(_v_%s), \"%%s\", ",
                        n->var_name, n->var_name);
                emit_c_string_expr(f, n->var_value ? n->var_value : "");
                fprintf(f, ");\n");
            }
            break;
        }

        case NODE_IF: {
            indent_c(f, depth);
            fprintf(f, "if (");
            emit_c_cond(f, &n->cond);
            fprintf(f, ") {\n");
            emit_c_nodes(f, n->then_body, n->then_count, depth+1);
            if (n->else_count > 0) {
                indent_c(f, depth);
                fprintf(f, "} else {\n");
                emit_c_nodes(f, n->else_body, n->else_count, depth+1);
            }
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_WHILE: {
            indent_c(f, depth);
            fprintf(f, "while (");
            emit_c_cond(f, &n->cond);
            fprintf(f, ") {\n");
            emit_c_nodes(f, n->then_body, n->then_count, depth+1);
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_FOR_IN: {
            indent_c(f, depth);
            fprintf(f, "{\n");
            indent_c(f, depth+1);
            fprintf(f, "const char *_list_%s[] = {", n->for_var);
            for (int i = 0; i < n->for_list_count; i++) {
                if (i > 0) fputs(", ", f);
                emit_c_string_expr(f, n->for_list[i]);
            }
            fprintf(f, "};\n");
            indent_c(f, depth+1);
            fprintf(f, "for (int _idx_%s = 0; _idx_%s < %d; _idx_%s++) {\n",
                    n->for_var, n->for_var, n->for_list_count, n->for_var);
            indent_c(f, depth+2);
            fprintf(f, "snprintf(_v_%s, sizeof(_v_%s), \"%%s\", _list_%s[_idx_%s]);\n",
                    n->for_var, n->for_var, n->for_var, n->for_var);
            emit_c_nodes(f, n->for_body, n->for_body_count, depth+2);
            indent_c(f, depth+1);
            fprintf(f, "}\n");
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_FOR_NUM: {
            indent_c(f, depth);
            fprintf(f, "for (long _n_%s = %d; _n_%s <= %d; _n_%s++) {\n",
                    n->for_var, n->for_from,
                    n->for_var, n->for_to,
                    n->for_var);
            indent_c(f, depth+1);
            fprintf(f, "snprintf(_v_%s, sizeof(_v_%s), \"%%ld\", _n_%s);\n",
                    n->for_var, n->for_var, n->for_var);
            emit_c_nodes(f, n->for_body, n->for_body_count, depth+1);
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_TIME: {
            indent_c(f, depth);
            fprintf(f, "{\n");
            indent_c(f, depth+1);
            fprintf(f, "struct timespec _ts0, _ts1;\n");
            indent_c(f, depth+1);
            fprintf(f, "clock_gettime(CLOCK_MONOTONIC, &_ts0);\n");
            emit_c_nodes(f, n->for_body, n->for_body_count, depth+1);
            indent_c(f, depth+1);
            fprintf(f, "clock_gettime(CLOCK_MONOTONIC, &_ts1);\n");
            indent_c(f, depth+1);
            fprintf(f, "long _ns = (_ts1.tv_sec-_ts0.tv_sec)*1000000000L + (_ts1.tv_nsec-_ts0.tv_nsec);\n");
            indent_c(f, depth+1);
            fprintf(f, "fprintf(stderr, \"time: %%ld ns\\n\", _ns);\n");
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_CMD: {
            indent_c(f, depth);
            fprintf(f, "{\n");
            indent_c(f, depth+1);
            fprintf(f, "char *_argv[] = {");
            for (int i = 0; i < n->argc; i++) {
                /* For commands with vars we need runtime strings — use strdup of expr */
                fprintf(f, "(char*)");
                emit_c_string_expr(f, n->args[i]);
                fprintf(f, ", ");
            }
            fprintf(f, "NULL};\n");
            indent_c(f, depth+1);
            fprintf(f, "_nxs_exec(_argv);\n");
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_EXIT: {
            indent_c(f, depth);
            fprintf(f, "exit(%d);\n", n->exit_code);
            break;
        }

        case NODE_REDIR: {
            /* Redirect: open file, dup2, execute inner node, restore */
            char redir_sym = (n->redir_kind == REDIR_APPEND) ? 'a' :
                             (n->redir_kind == REDIR_IN)     ? 'r' : 'w';
            indent_c(f, depth);
            fprintf(f, "{\n");
            indent_c(f, depth+1);
            fprintf(f, "FILE *_rf = fopen(");
            emit_c_string_expr(f, n->redir_file);
            fprintf(f, ", \"%c\"); int _rt = %d;\n",
                redir_sym,
                (n->redir_kind == REDIR_IN) ? STDIN_FILENO : STDOUT_FILENO);
            indent_c(f, depth+1);
            fprintf(f, "int _rs = dup(_rt);\n");
            indent_c(f, depth+1);
            fprintf(f, "if(_rf){dup2(fileno(_rf),_rt);fclose(_rf);}\n");
            if (n->redir_cmd) emit_c_node(f, n->redir_cmd, depth+1);
            indent_c(f, depth+1);
            fprintf(f, "fflush(NULL); dup2(_rs,_rt); close(_rs);\n");
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_PIPELINE: {
            /* Pipeline: fork+pipe chain */
            indent_c(f, depth);
            fprintf(f, "{\n");
            indent_c(f, depth+1);
            fprintf(f, "_nxs_pipeline_begin(%d);\n", n->pipeline_count);
            for (int i = 0; i < n->pipeline_count; i++) {
                indent_c(f, depth+1);
                fprintf(f, "if(_nxs_pipeline_stage(%d,%d)) {\n", i, n->pipeline_count);
                if (n->pipeline_cmds[i]) emit_c_node(f, n->pipeline_cmds[i], depth+2);
                indent_c(f, depth+2);
                fprintf(f, "_nxs_pipeline_child_exit();\n");
                indent_c(f, depth+1);
                fprintf(f, "}\n");
            }
            indent_c(f, depth+1);
            fprintf(f, "_nxs_pipeline_wait(%d);\n", n->pipeline_count);
            indent_c(f, depth);
            fprintf(f, "}\n");
            break;
        }

        case NODE_BLOCK: {
            /* Subshell: fork + run block in child */
            indent_c(f, depth);
            fprintf(f, "{ fflush(NULL); pid_t _sp = fork();\n");
            indent_c(f, depth);
            fprintf(f, "if (_sp == 0) {\n");
            emit_c_nodes(f, n->for_body, n->for_body_count, depth+1);
            indent_c(f, depth+1);
            fprintf(f, "exit(0);\n");
            indent_c(f, depth);
            fprintf(f, "} else if (_sp > 0) { int _ss; waitpid(_sp,&_ss,0); } }\n");
            break;
        }

        default: break;
    }
}

static void emit_c_nodes(FILE *f, const Node *nodes, size_t count, int depth) {
    for (size_t i = 0; i < count; i++) emit_c_node(f, &nodes[i], depth);
}

/* =========================================================
   Header / preamble
   ========================================================= */

static void emit_c_header(FILE *f) {
    fputs(
        "#define _POSIX_C_SOURCE 200809L\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdarg.h>\n"
        "#include <unistd.h>\n"
        "#include <fcntl.h>\n"
        "#include <sys/stat.h>\n"
        "#include <sys/wait.h>\n"
        "#include <time.h>\n"
        "\n"
        "/* -- runtime helpers -- */\n"
        "static char _nxs_interp_buf[4096];\n"
        "#define _nxs_interp(fmt, ...) \\\n"
        "    (snprintf(_nxs_interp_buf, sizeof(_nxs_interp_buf), fmt, ##__VA_ARGS__), \\\n"
        "     _nxs_interp_buf)\n"
        "\n"
        "static void _nxs_echo_impl(int argc, ...) {\n"
        "    va_list ap; va_start(ap, argc);\n"
        "    for (int i=0;i<argc;i++) {\n"
        "        if (i) putchar(' ');\n"
        "        fputs(va_arg(ap, const char *), stdout);\n"
        "    }\n"
        "    putchar('\\n');\n"
        "    va_end(ap);\n"
        "}\n"
        "#define _nxs_echo(...) \\\n"
        "    _nxs_echo_impl((int)(sizeof((const char*[]){__VA_ARGS__})/sizeof(const char*)), __VA_ARGS__)\n"
        "\n"
        "static void _nxs_exec(char *const argv[]) {\n"
        "    fflush(NULL);\n"
        "    pid_t pid = fork();\n"
        "    if (pid == 0) {\n"
        "        execvp(argv[0], argv);\n"
        "        perror(argv[0]); exit(127);\n"
        "    } else if (pid > 0) {\n"
        "        int st; waitpid(pid, &st, 0);\n"
        "    }\n"
        "}\n"
        "\n"
        "/* pipeline support */\n"
        "static pid_t _pipe_pids[64];\n"
        "static int   _pipe_count;\n"
        "static int   _pipe_prev_read;\n"
        "static int   _pipe_fd[2];\n"
        "static int   _pipe_stage_i;\n"
        "\n"
        "static void _nxs_pipeline_begin(int n) {\n"
        "    _pipe_count = n; _pipe_prev_read = STDIN_FILENO; _pipe_stage_i = 0;\n"
        "}\n"
        "/* returns 1 if we are in the child for this stage */\n"
        "static int _nxs_pipeline_stage(int i, int total) {\n"
        "    int out_fd = STDOUT_FILENO;\n"
        "    if (i < total - 1) { pipe(_pipe_fd); out_fd = _pipe_fd[1]; }\n"
        "    fflush(NULL);\n"
        "    pid_t pid = fork();\n"
        "    if (pid == 0) { /* child */\n"
        "        if (_pipe_prev_read != STDIN_FILENO) { dup2(_pipe_prev_read, STDIN_FILENO); close(_pipe_prev_read); }\n"
        "        if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }\n"
        "        if (i < total - 1) close(_pipe_fd[0]);\n"
        "        return 1;\n"
        "    }\n"
        "    if (_pipe_prev_read != STDIN_FILENO) close(_pipe_prev_read);\n"
        "    if (i < total - 1) { close(_pipe_fd[1]); _pipe_prev_read = _pipe_fd[0]; }\n"
        "    if (_pipe_count < 64) _pipe_pids[_pipe_stage_i++] = pid;\n"
        "    return 0;\n"
        "}\n"
        "static void _nxs_pipeline_child_exit(void) { exit(0); }\n"
        "static void _nxs_pipeline_wait(int n) {\n"
        "    for (int i = 0; i < n && i < 64; i++) { int st; waitpid(_pipe_pids[i], &st, 0); }\n"
        "}\n"
        "\n",
    f);
}

static void emit_c_vars(FILE *f) {
    for (int i = 0; i < var_count; i++)
        fprintf(f, "static char _v_%s[4096] = \"\";\n", var_names[i]);
    if (var_count) fputs("\n", f);
}

/* =========================================================
   Public entry point
   ========================================================= */

int codegen_emit(Arena *arena, const AST *ast, const char *out_path) {
    (void)arena;
    var_count = 0;
    collect_vars_nodes(ast->nodes, ast->count);

    /* Write temp C file */
    char tmp_c[256];
    snprintf(tmp_c, sizeof(tmp_c), "/tmp/nxsc_%d.c", (int)getpid());

    FILE *f = fopen(tmp_c, "w");
    if (!f) { perror(tmp_c); return 1; }

    emit_c_header(f);
    emit_c_vars(f);
    fputs("int main(void) {\n", f);
    emit_c_nodes(f, ast->nodes, ast->count, 1);
    fputs("    return 0;\n}\n", f);
    fclose(f);

    /* Invoke gcc */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "gcc -O2 -o %s %s -lpthread 2>&1",
             out_path, tmp_c);
    int rc = system(cmd);
    unlink(tmp_c);
    return rc;
}
