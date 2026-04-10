#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "sysinfo.h"
#include "threadpool.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "debug.h"
#include "interp.h"

/* ------------------------------------------------------------------ */
/* Compile job (one source file per job, run in thread pool)           */
/* ------------------------------------------------------------------ */

typedef struct CompileJob {
    const char *in_path;
    const char *out_path;
    int         result;   /* 0 = ok */
} CompileJob;

static char *read_source(const char *path, long *out_sz) {
    FILE *f = fopen(path, "rb");
    char *src;
    long sz;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);

    src = malloc(sz + 1);
    if (!src) {
        fclose(f);
        return NULL;
    }

    if (fread(src, 1, sz, f) != (size_t)sz) {
        fclose(f);
        free(src);
        return NULL;
    }
    fclose(f);
    src[sz] = '\0';
    if (out_sz) *out_sz = sz;
    return src;
}

static void compile_job(void *arg) {
    CompileJob *job = arg;

    /* Read source file */
    long sz = 0;
    char *src = read_source(job->in_path, &sz);
    if (!src) {
        fprintf(stderr, "nxsc: cannot open '%s'\n", job->in_path);
        job->result = 1;
        return;
    }

    Arena arena = arena_new(1024 * 1024);

    TokenList tl  = lex(&arena, src, sz);
    AST       ast = parse(&arena, &tl);
    int       rc  = codegen_emit(&arena, &ast, job->out_path);

    arena_free(&arena);
    free(src);
    job->result = rc;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Niaxinus Compiler v0.2\n"
        "Usage:\n"
        "  %s <source.nxs> [output]\n"
        "  %s --run <source.nxs>\n"
        "  %s --tokens <source.nxs>\n"
        "  %s --ast <source.nxs>\n", prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "--run") == 0 ||
        strcmp(argv[1], "--tokens") == 0 ||
        strcmp(argv[1], "--ast") == 0) {
        long sz = 0;
        char *src;
        Arena arena;
        TokenList tl;
        AST ast;
        int rc = 0;

        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }

        src = read_source(argv[2], &sz);
        if (!src) {
            fprintf(stderr, "nxsc: cannot open '%s'\n", argv[2]);
            return 1;
        }

        arena = arena_new(1024 * 1024);
        tl = lex(&arena, src, (size_t)sz);
        ast = parse(&arena, &tl);

        if (strcmp(argv[1], "--tokens") == 0) {
            dump_tokens(stdout, &tl);
        } else if (strcmp(argv[1], "--ast") == 0) {
            dump_ast(stdout, &ast);
        } else {
            rc = interp_run(&ast);
        }

        arena_free(&arena);
        free(src);
        return rc;
    }

    /* --- System probe --- */
    printf("niaxinus compiler v0.2 — system probe:\n");
    SysInfo si = sysinfo_probe();
    sysinfo_print(&si);

    int nthreads = si.logical_cores;
    if (nthreads < 1) nthreads = 1;
    printf("  Threads : %d (torrent-mode max parallelism)\n\n", nthreads);

    /* --- Build job list --- */
    /* Syntax: nxsc file.nxs [output]  OR  nxsc file1.nxs file2.nxs ... */
    int njobs;
    int explicit_output = 0;
    if (argc == 3) {
        const char *dot = strrchr(argv[2], '.');
        if (!dot || strcmp(dot, ".nxs") != 0) {
            njobs = 1;
            explicit_output = 1;
        } else {
            njobs = 2;
        }
    } else {
        njobs = argc - 1;
    }
    CompileJob *jobs = calloc(njobs, sizeof(CompileJob));

    for (int i = 0; i < njobs; i++) {
        jobs[i].in_path = argv[i + 1];

        char *out;
        if (explicit_output && i == 0) {
            out = strdup(argv[2]);
        } else {
            const char *base = strrchr(argv[i + 1], '/');
            base = base ? base + 1 : argv[i + 1];
            out = malloc(strlen(base) + 8);
            strcpy(out, base);
            char *dot = strrchr(out, '.');
            if (dot) *dot = '\0';
        }
        jobs[i].out_path = out;
    }

    /* --- Launch thread pool --- */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ThreadPool *tp = threadpool_create(nthreads);

    for (int i = 0; i < njobs; i++)
        threadpool_submit(tp, compile_job, &jobs[i]);

    threadpool_wait(tp);
    threadpool_destroy(tp);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) * 1e-9;

    /* --- Report --- */
    int errors = 0;
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].result == 0)
            printf("  compiled: %s  →  %s\n",
                   jobs[i].in_path, jobs[i].out_path);
        else {
            fprintf(stderr, "  FAILED  : %s\n", jobs[i].in_path);
            errors++;
        }
    }
    printf("\n%.3f ms  (%d file(s), %d thread(s))\n",
           elapsed * 1000.0, njobs, nthreads);

    /* cleanup */
    for (int i = 0; i < njobs; i++)
        free((void*)jobs[i].out_path);
    free(jobs);

    return errors ? 1 : 0;
}
