#include "arena.h"
#include <stdlib.h>
#include <string.h>

Arena arena_new(size_t cap) {
    Arena a;
    a.base = malloc(cap);
    a.used = 0;
    a.cap  = cap;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    /* 8-byte align */
    size_t aligned = (size + 7) & ~(size_t)7;
    if (a->used + aligned > a->cap) {
        a->cap  = (a->cap + aligned) * 2;
        a->base = realloc(a->base, a->cap);
    }
    void *ptr = a->base + a->used;
    a->used  += aligned;
    return ptr;
}

char *arena_strdup(Arena *a, const char *s, size_t len) {
    char *dst = arena_alloc(a, len + 1);
    memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

void arena_free(Arena *a) {
    free(a->base);
    a->base = NULL;
    a->used = 0;
    a->cap  = 0;
}
