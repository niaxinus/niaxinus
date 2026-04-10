#define _POSIX_C_SOURCE 200809L
#include "interp.h"

#include "lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_VARS 256

typedef struct {
    char name[64];
    char value[4096];
    int  exported;
} InterpVar;

typedef struct {
    InterpVar vars[MAX_VARS];
    int var_count;
    int should_exit;
    int exit_code;
    int last_status;
    /* positional params */
    const char *argv0;
    char **posargs;
    int posarg_count;
} InterpState;

static InterpVar *find_var(InterpState *st, const char *name) {
    for (int i = 0; i < st->var_count; i++) {
        if (strcmp(st->vars[i].name, name) == 0) return &st->vars[i];
    }
    return NULL;
}

static const char *get_var(InterpState *st, const char *name) {
    /* special variables */
    if (strcmp(name, "?") == 0) {
        static char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%d", st->last_status);
        return sbuf;
    }
    if (strcmp(name, "$") == 0) {
        static char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%d", (int)getpid());
        return sbuf;
    }
    if (strcmp(name, "0") == 0) return st->argv0 ? st->argv0 : "";
    if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0') {
        int idx = name[0] - '0';
        if (idx <= st->posarg_count && st->posargs) return st->posargs[idx - 1];
        return "";
    }
    InterpVar *var = find_var(st, name);
    if (var) return var->value;
    /* fall back to environment */
    const char *env = getenv(name);
    return env ? env : "";
}

static void set_var(InterpState *st, const char *name, const char *value) {
    InterpVar *var = find_var(st, name);
    if (!var) {
        if (st->var_count >= MAX_VARS) return;
        var = &st->vars[st->var_count++];
        snprintf(var->name, sizeof(var->name), "%s", name);
        var->exported = 0;
    }
    snprintf(var->value, sizeof(var->value), "%s", value ? value : "");
    /* sync exported vars to environment */
    if (var->exported) setenv(name, var->value, 1);
}

static void export_var(InterpState *st, const char *name, const char *value) {
    set_var(st, name, value);
    InterpVar *var = find_var(st, name);
    if (var) { var->exported = 1; setenv(name, var->value, 1); }
}

static void unset_var(InterpState *st, const char *name) {
    for (int i = 0; i < st->var_count; i++) {
        if (strcmp(st->vars[i].name, name) == 0) {
            unsetenv(name);
            /* Shift remaining vars down */
            for (int j = i; j < st->var_count - 1; j++)
                st->vars[j] = st->vars[j + 1];
            st->var_count--;
            return;
        }
    }
    unsetenv(name);
}

/* Expand ${...} expression: supports ${var}, ${var:-def}, ${var:+val},
   ${var:?msg}, ${#var}, ${var%pat} (suffix strip), ${var#pat} (prefix strip) */
static void expand_brace(InterpState *st, const char *expr, char *out, size_t out_sz) {
    if (expr[0] == '#') {
        /* ${#var} → length */
        const char *val = get_var(st, expr + 1);
        snprintf(out, out_sz, "%zu", strlen(val));
        return;
    }
    /* Find operator position */
    size_t ni = 0;
    char name[64];
    while (expr[ni] && expr[ni] != ':' && expr[ni] != '%' && expr[ni] != '#' && ni < 63) {
        name[ni] = expr[ni]; ni++;
    }
    name[ni] = '\0';
    const char *val = get_var(st, name);
    const char *op = expr + ni;

    if (*op == '\0') {
        /* plain ${var} */
        snprintf(out, out_sz, "%s", val);
    } else if (op[0] == ':' && op[1] == '-') {
        /* ${var:-default} */
        snprintf(out, out_sz, "%s", (val && *val) ? val : op + 2);
    } else if (op[0] == ':' && op[1] == '+') {
        /* ${var:+val} — val if set, empty if not */
        snprintf(out, out_sz, "%s", (val && *val) ? op + 2 : "");
    } else if (op[0] == ':' && op[1] == '?') {
        /* ${var:?msg} — error if unset */
        if (!val || !*val) {
            fprintf(stderr, "%s: %s\n", name, *(op + 2) ? op + 2 : "parameter null or not set");
            out[0] = '\0';
        } else {
            snprintf(out, out_sz, "%s", val);
        }
    } else if (op[0] == '#') {
        /* ${var#pat} — strip shortest prefix matching pat (literal only) */
        const char *pat = op + 1;
        size_t plen = strlen(pat);
        if (strncmp(val, pat, plen) == 0)
            snprintf(out, out_sz, "%s", val + plen);
        else
            snprintf(out, out_sz, "%s", val);
    } else if (op[0] == '%') {
        /* ${var%pat} — strip shortest suffix matching pat (literal only) */
        const char *pat = op + 1;
        size_t vlen = strlen(val);
        size_t plen = strlen(pat);
        if (vlen >= plen && strcmp(val + vlen - plen, pat) == 0) {
            size_t keep = vlen - plen;
            if (keep >= out_sz) keep = out_sz - 1;
            memcpy(out, val, keep);
            out[keep] = '\0';
        } else {
            snprintf(out, out_sz, "%s", val);
        }
    } else {
        snprintf(out, out_sz, "%s", val);
    }
}

static void eval_string(InterpState *st, const char *raw, char *out, size_t out_sz) {
    size_t oi = 0;
    if (!raw) { if (out_sz) out[0] = '\0'; return; }
    for (size_t i = 0; raw[i] && oi + 1 < out_sz; ) {
        if (raw[i] == '\\' && raw[i+1]) {
            /* basic escape: \n \t \\ */
            char c = raw[++i];
            switch (c) {
                case 'n':  out[oi++] = '\n'; break;
                case 't':  out[oi++] = '\t'; break;
                default:   out[oi++] = c;    break;
            }
            i++;
        } else if (raw[i] == '$' && raw[i+1] == '{') {
            /* ${...} brace expansion */
            i += 2;
            size_t start = i;
            int depth = 1;
            while (raw[i] && depth > 0) {
                if (raw[i] == '{') depth++;
                else if (raw[i] == '}') depth--;
                if (depth > 0) i++;
                else i++; /* consume closing } */
            }
            size_t elen = (i - 1) - start;
            char expr[256]; if (elen > 255) elen = 255;
            memcpy(expr, raw + start, elen); expr[elen] = '\0';
            char ebuf[4096];
            expand_brace(st, expr, ebuf, sizeof(ebuf));
            for (size_t j = 0; ebuf[j] && oi + 1 < out_sz; j++) out[oi++] = ebuf[j];
        } else if (raw[i] == '$' && raw[i+1] == '(') {
            /* $(...) command substitution — skip unsupported for now */
            out[oi++] = raw[i++];
        } else if (raw[i] == '$' && (isalnum((unsigned char)raw[i+1]) || raw[i+1] == '_' || raw[i+1] == '?' || raw[i+1] == '$' || raw[i+1] == '!')) {
            char name[64];
            int ni = 0;
            i++;
            char c0 = raw[i];
            if (c0 == '?' || c0 == '$' || c0 == '!') {
                name[ni++] = raw[i++];
            } else {
                while (raw[i] && (isalnum((unsigned char)raw[i]) || raw[i] == '_') && ni < 63)
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
static int exec_node(InterpState *st, const Node *n);

/* Run shell builtins; returns -1 if not a builtin, else exit code */
static int try_builtin(InterpState *st, char **argv, int argc) {
    if (!argv[0]) return -1;
    const char *cmd = argv[0];

    /* cd */
    if (strcmp(cmd, "cd") == 0) {
        const char *dir = (argc > 1 && argv[1]) ? argv[1] : get_var(st, "HOME");
        if (!dir || !*dir) dir = "/";
        /* cd - goes to OLDPWD and prints it */
        int print_dir = 0;
        char target[4096];
        if (strcmp(dir, "-") == 0) {
            const char *op = get_var(st, "OLDPWD");
            snprintf(target, sizeof(target), "%s", (op && *op) ? op : "/");
            dir = target;
            print_dir = 1;
        }
        /* save current dir as OLDPWD before changing */
        char old[4096];
        if (getcwd(old, sizeof(old))) set_var(st, "OLDPWD", old);
        int rc = chdir(dir);
        if (rc != 0) { perror(dir); st->last_status = 1; return 1; }
        /* update PWD */
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            set_var(st, "PWD", cwd);
            if (print_dir) puts(cwd);
        }
        st->last_status = 0; return 0;
    }
    /* pwd */
    if (strcmp(cmd, "pwd") == 0) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) { puts(cwd); st->last_status = 0; return 0; }
        st->last_status = 1; return 1;
    }
    /* export [name=value | name] */
    if (strcmp(cmd, "export") == 0) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                export_var(st, argv[i], eq + 1);
                *eq = '=';
            } else {
                export_var(st, argv[i], get_var(st, argv[i]));
            }
        }
        st->last_status = 0; return 0;
    }
    /* unset */
    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < argc; i++) unset_var(st, argv[i]);
        st->last_status = 0; return 0;
    }
    /* true / false */
    if (strcmp(cmd, "true") == 0 || strcmp(cmd, ":") == 0) { st->last_status = 0; return 0; }
    if (strcmp(cmd, "false") == 0) { st->last_status = 1; return 1; }
    /* echo (also handle as builtin for pipeline children) */
    if (strcmp(cmd, "echo") == 0) {
        int newline = 1;
        int start = 1;
        if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
        for (int i = start; i < argc; i++) {
            if (i > start) putchar(' ');
            fputs(argv[i], stdout);
        }
        if (newline) putchar('\n');
        st->last_status = 0; return 0;
    }
    /* read varname */
    if (strcmp(cmd, "read") == 0) {
        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) { st->last_status = 1; return 1; }
        /* strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (argc > 1) set_var(st, argv[1], line);
        st->last_status = 0; return 0;
    }
    /* shift [n] */
    if (strcmp(cmd, "shift") == 0) {
        int n = (argc > 1) ? atoi(argv[1]) : 1;
        if (n > st->posarg_count) n = st->posarg_count;
        if (st->posargs) {
            st->posargs += n;
            st->posarg_count -= n;
        }
        st->last_status = 0; return 0;
    }
    /* test / [ */
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0) {
        /* minimal: test -z str, test -n str, test str = str, test str != str
           test -f file, test -d dir, test -e file */
        int end = argc - (strcmp(cmd, "[") == 0 ? 1 : 0); /* skip trailing ] */
        int result = 0;
        if (end - 1 == 2 && strcmp(argv[1], "-z") == 0) result = (strlen(argv[2]) == 0);
        else if (end - 1 == 2 && strcmp(argv[1], "-n") == 0) result = (strlen(argv[2]) > 0);
        else if (end - 1 == 2 && strcmp(argv[1], "-f") == 0) { struct stat sb; result = (stat(argv[2], &sb) == 0 && S_ISREG(sb.st_mode)); }
        else if (end - 1 == 2 && strcmp(argv[1], "-d") == 0) { struct stat sb; result = (stat(argv[2], &sb) == 0 && S_ISDIR(sb.st_mode)); }
        else if (end - 1 == 2 && strcmp(argv[1], "-e") == 0) { struct stat sb; result = (stat(argv[2], &sb) == 0); }
        else if (end - 1 == 3 && strcmp(argv[2], "=") == 0)  result = (strcmp(argv[1], argv[3]) == 0);
        else if (end - 1 == 3 && strcmp(argv[2], "!=") == 0) result = (strcmp(argv[1], argv[3]) != 0);
        else if (end - 1 == 3 && strcmp(argv[2], "-eq") == 0) result = (atol(argv[1]) == atol(argv[3]));
        else if (end - 1 == 3 && strcmp(argv[2], "-ne") == 0) result = (atol(argv[1]) != atol(argv[3]));
        else if (end - 1 == 3 && strcmp(argv[2], "-lt") == 0) result = (atol(argv[1]) <  atol(argv[3]));
        else if (end - 1 == 3 && strcmp(argv[2], "-gt") == 0) result = (atol(argv[1]) >  atol(argv[3]));
        else if (end - 1 == 3 && strcmp(argv[2], "-le") == 0) result = (atol(argv[1]) <= atol(argv[3]));
        else if (end - 1 == 3 && strcmp(argv[2], "-ge") == 0) result = (atol(argv[1]) >= atol(argv[3]));
        else if (end - 1 == 1) result = (strlen(argv[1]) > 0); /* test str */
        st->last_status = result ? 0 : 1; return st->last_status;
    }
    return -1; /* not a builtin */
}

static int exec_cmd(InterpState *st, const Node *n) {
    char *argv[256];
    if (n->argc <= 0) return 0;
    int argc = (n->argc < 255) ? n->argc : 255;
    for (int i = 0; i < argc; i++) {
        char buf[4096];
        eval_string(st, n->args[i], buf, sizeof(buf));
        argv[i] = strdup(buf);
    }
    argv[argc] = NULL;

    /* Try builtins first */
    int rc = try_builtin(st, argv, argc);
    if (rc >= 0) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        return rc;
    }

    fflush(NULL);
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

    for (int i = 0; i < argc; i++) free(argv[i]);
    st->last_status = status;
    return status;
}

/* Build argv for a NODE_CMD or NODE_ECHO node and exec it (child process only) */
static void exec_child_node(InterpState *st, const Node *n) {
    char *argv[256];
    const char **src = n->args;
    int argc = n->argc;
    /* For NODE_ECHO prepend "echo" */
    const char *echo_cmd = "echo";
    if (n->kind == NODE_ECHO) { src = NULL; /* handled below */ }
    if (n->kind == NODE_ECHO) {
        argv[0] = (char *)echo_cmd;
        for (int i = 0; i < argc && i < 254; i++) {
            char *buf = malloc(4096);
            eval_string(st, n->args[i], buf, 4096);
            argv[i+1] = buf;
        }
        argv[argc+1] = NULL;
        execvp("echo", argv);
        _exit(127);
    }
    for (int i = 0; i < argc && i < 255; i++) {
        char *buf = malloc(4096);
        eval_string(st, src[i], buf, 4096);
        argv[i] = buf;
    }
    argv[argc] = NULL;
    execvp(argv[0], argv);
    _exit(127);
}

/* Execute a single command in the pipeline (child side) with fd remapping */
static void run_pipeline_child(InterpState *st, const Node *n, int in_fd, int out_fd) {
    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
    exec_child_node(st, n);
}

static int exec_pipeline(InterpState *st, const Node *n) {
    int count = n->pipeline_count;
    if (count == 0) return 0;
    if (count == 1) return exec_node(st, n->pipeline_cmds[0]);

    int prev_read = STDIN_FILENO;
    pid_t pids[64];
    int np = 0;

    fflush(NULL);
    for (int i = 0; i < count; i++) {
        int pfd[2] = {-1, -1};
        int out_fd = STDOUT_FILENO;
        if (i < count - 1) {
            pipe(pfd);
            out_fd = pfd[1];
        }
        pid_t pid = fork();
        if (pid == 0) {
            if (i < count - 1) close(pfd[0]);
            run_pipeline_child(st, n->pipeline_cmds[i], prev_read, out_fd);
            _exit(127);
        }
        if (prev_read != STDIN_FILENO) close(prev_read);
        if (i < count - 1) {
            close(pfd[1]);
            prev_read = pfd[0];
        }
        if (np < 64) pids[np++] = pid;
    }

    int status = 0;
    for (int i = 0; i < np; i++) {
        int raw = 0;
        waitpid(pids[i], &raw, 0);
        if (i == np - 1 && WIFEXITED(raw)) status = WEXITSTATUS(raw);
    }
    st->last_status = status;
    return status;
}

static int exec_redir(InterpState *st, const Node *n) {
    char file[4096];
    eval_string(st, n->redir_file, file, sizeof(file));

    int flags, mode = 0644;
    switch (n->redir_kind) {
        case REDIR_OUT:    flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case REDIR_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case REDIR_IN:     flags = O_RDONLY; break;
        default:           flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    int fd = open(file, flags, mode);
    if (fd < 0) { perror(file); st->last_status = 1; return 1; }

    /* Save original fd */
    int target_fd = (n->redir_kind == REDIR_IN) ? STDIN_FILENO : STDOUT_FILENO;
    int saved = dup(target_fd);

    dup2(fd, target_fd);
    close(fd);

    exec_node(st, n->redir_cmd);

    /* Flush stdio buffers before restoring the fd */
    fflush(NULL);

    /* Restore */
    dup2(saved, target_fd);
    close(saved);
    return st->last_status;
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
        case NODE_PIPELINE:
            return exec_pipeline(st, n);
        case NODE_REDIR:
            return exec_redir(st, n);
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
