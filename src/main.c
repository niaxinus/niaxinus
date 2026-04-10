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

/* ------------------------------------------------------------------ */
/* Compile job (one source file per job, run in thread pool)           */
/* ------------------------------------------------------------------ */

typedef struct CompileJob {
    const char *in_path;
    const char *out_path;
    int         result;   /* 0 = ok */
} CompileJob;

static void compile_job(void *arg) {
    CompileJob *job = arg;

    /* Read source file */
    FILE *f = fopen(job->in_path, "rb");
    if (!f) {
        fprintf(stderr, "nxsc: cannot open '%s'\n", job->in_path);
        job->result = 1;
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = malloc(sz + 1);
    fread(src, 1, sz, f);
    fclose(f);
    src[sz] = '\0';

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
        "Niaxinus Compiler v0.1\n"
        "Usage: %s <source.nxs> [output]\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    /* --- System probe --- */
    printf("niaxinus compiler v0.1 — system probe:\n");
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
