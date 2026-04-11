#define _POSIX_C_SOURCE 200809L
#include "repl.h"
#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* Optional readline support — detected at compile time */
#ifdef HAVE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>

/* Global depth for Tab handler (updated by repl_run) */
static int repl_depth_global = 0;

/* ------------------------------------------------------------------ */
/* Tab completion                                                       */
/* ------------------------------------------------------------------ */

static const char *nxs_keywords[] = {
    /* control flow */
    "if", "elif", "else", "while", "for", "in", "time",
    /* builtins */
    "echo", "cd", "pwd", "export", "unset", "read", "shift",
    "true", "false", "test", "exit",
    /* common external commands */
    "ls", "cat", "grep", "sort", "head", "tail", "wc",
    "mkdir", "rm", "cp", "mv", "chmod", "find", "awk", "sed",
    "date", "uname", "ps", "kill", "which",
    /* REPL specials */
    ":help", ":quit", ":reset", ":status",
    NULL
};

static char *nxs_keyword_generator(const char *text, int state) {
    static int idx;
    static size_t tlen;
    if (!state) { idx = 0; tlen = strlen(text); }
    while (nxs_keywords[idx]) {
        const char *kw = nxs_keywords[idx++];
        if (strncmp(kw, text, tlen) == 0)
            return strdup(kw);
    }
    return NULL;
}

static char **nxs_completion(const char *text, int start, int end) {
    (void)end;
    /* if not the first word, fall back to filename completion */
    if (start > 0) {
        rl_attempted_completion_over = 0;
        return NULL;
    }
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, nxs_keyword_generator);
}

/* Tab handler: indent inside blocks, complete at top level */
static int nxs_tab_handler(int count, int key) {
    (void)count; (void)key;
    if (repl_depth_global > 0) {
        /* insert 4 spaces per block level if line is empty/whitespace-only */
        int pos = rl_point;
        int only_ws = 1;
        for (int i = 0; i < pos; i++) {
            if (rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t') {
                only_ws = 0;
                break;
            }
        }
        /* always insert 4 spaces on Tab in block context */
        (void)only_ws;
        rl_insert_text("    ");
        return 0;
    }
    /* top-level: normal completion */
    return rl_complete(0, '\t');
}

static void repl_readline_init(void) {
    rl_readline_name = "nxsc";
    rl_attempted_completion_function = nxs_completion;
    rl_bind_key('\t', nxs_tab_handler);
}
#endif

#define REPL_LINE_MAX  4096
#define REPL_BUF_MAX   (64 * 1024)

/* ------------------------------------------------------------------ */
/* Prompt helpers                                                       */
/* ------------------------------------------------------------------ */

static char *repl_readline(const char *prompt) {
#ifdef HAVE_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;   /* caller must free() */
#else
    static char buf[REPL_LINE_MAX];
    if (isatty(fileno(stdin))) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    /* strip trailing newline */
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    return buf;   /* static buf — valid until next call */
#endif
}

static void repl_free_line(char *line) {
#ifdef HAVE_READLINE
    free(line);
#else
    (void)line;
#endif
}

/* ------------------------------------------------------------------ */
/* Block-depth detector                                                 */
/* Returns how many new open blocks a line introduces:                 */
/*   "if ...:"  →  +1                                                  */
/*   ""         →  -1  (empty line closes a pending block)             */
/* ------------------------------------------------------------------ */

static int line_opens_block(const char *line) {
    /* skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return -1;   /* empty line */

    /* block keywords followed by ':' at end */
    const char *kw[] = { "if", "elif", "else", "while", "for", "time", NULL };
    for (int i = 0; kw[i]; i++) {
        size_t klen = strlen(kw[i]);
        if (strncmp(line, kw[i], klen) == 0 &&
            (line[klen] == '\0' || line[klen] == ' ' || line[klen] == ':')) {
            /* check that the line ends with ':' */
            const char *end = line + strlen(line) - 1;
            while (end > line && (*end == ' ' || *end == '\t')) end--;
            if (*end == ':') return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Help text                                                            */
/* ------------------------------------------------------------------ */

static void print_help(void) {
    printf(
        "\n"
        "  Niaxinus REPL — interaktív értelmező\n"
        "  ─────────────────────────────────────\n"
        "  Írj NXS/Bash kódot — azonnal lefut.\n"
        "  Változók megmaradnak sor-sor között.\n"
        "\n"
        "  Speciális parancsok:\n"
        "    :help      ez a súgó\n"
        "    :q  :quit  kilépés\n"
        "    :reset     változók törlése (új munkamenet)\n"
        "    :status    utolsó $? értéke\n"
        "    Ctrl-D     kilépés\n"
        "\n"
        "  Példák:\n"
        "    nxs> x = \"hello\"\n"
        "    nxs> echo $x\n"
        "    nxs> if [ $x = \"hello\" ]:\n"
        "    ...>     echo \"egyezik\"\n"
        "    ...> \n"
        "\n"
    );
}

/* ------------------------------------------------------------------ */
/* Main REPL loop                                                       */
/* ------------------------------------------------------------------ */

void repl_run(void) {
#ifdef HAVE_READLINE
    repl_readline_init();
#endif
    InterpHandle *h = interp_state_new();
    if (!h) { fprintf(stderr, "nxsc: repl: out of memory\n"); return; }

    /* welcome banner (only when interactive) */
    if (isatty(fileno(stdin))) {
        printf("Niaxinus REPL v0.3  — írj :help a súgóhoz, :q a kilépéshez\n");
    }

    /* accumulated source buffer for multi-line blocks */
    char   *accum     = malloc(REPL_BUF_MAX);
    size_t  accum_len = 0;
    int     depth     = 0;   /* open block depth */

    if (!accum) { fprintf(stderr, "nxsc: repl: out of memory\n"); interp_state_free(h); return; }

    for (;;) {
        const char *prompt = (depth > 0) ? "...> " : "nxs> ";
        char *line = repl_readline(prompt);

        /* Ctrl-D / EOF */
        if (!line) {
            if (isatty(fileno(stdin))) printf("\n");
            break;
        }

        /* --- special REPL commands (only at top level) --- */
        if (depth == 0) {
            /* skip leading spaces */
            const char *trimmed = line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

            if (strcmp(trimmed, ":q") == 0 || strcmp(trimmed, ":quit") == 0) {
                repl_free_line(line);
                break;
            }
            if (strcmp(trimmed, ":help") == 0) {
                print_help();
                repl_free_line(line);
                continue;
            }
            if (strcmp(trimmed, ":reset") == 0) {
                interp_state_free(h);
                h = interp_state_new();
                printf("  [munkamenet törölve]\n");
                repl_free_line(line);
                continue;
            }
            if (strcmp(trimmed, ":status") == 0) {
                printf("  $? = %d\n", interp_last_status(h));
                repl_free_line(line);
                continue;
            }
        }

        /* --- accumulate line into buffer --- */
        size_t llen = strlen(line);
        int is_empty = (llen == 0);

        if (!is_empty && accum_len + llen + 2 < REPL_BUF_MAX) {
            memcpy(accum + accum_len, line, llen);
            accum_len += llen;
            accum[accum_len++] = '\n';
            accum[accum_len]   = '\0';
        }

        /* --- block depth tracking (use raw line, before it's freed) --- */
        int delta = is_empty ? -1 : line_opens_block(line);
        repl_free_line(line);

        if (delta > 0) {
            depth++;
#ifdef HAVE_READLINE
            repl_depth_global = depth;
#endif
            continue;
        }
        if (delta < 0 && depth > 0) {
            /* empty line closes block */
            depth--;
#ifdef HAVE_READLINE
            repl_depth_global = depth;
#endif
            if (depth > 0) continue;
        }

        /* --- execute when depth reaches 0 --- */
        if (depth == 0 && accum_len > 0) {
            int rc = interp_exec_src(h, accum, accum_len);
            accum_len = 0;
            accum[0]  = '\0';

            if (interp_should_exit(h)) {
                if (isatty(fileno(stdin)))
                    printf("  [kilépés: %d]\n", rc);
                break;
            }
        }
    }

    free(accum);
    interp_state_free(h);
}
