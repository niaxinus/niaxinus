#pragma once
#include <stddef.h>

typedef struct Arena {
    char   *base;
    size_t  used;
    size_t  cap;
} Arena;

Arena  arena_new(size_t cap);
void  *arena_alloc(Arena *a, size_t size);
char  *arena_strdup(Arena *a, const char *s, size_t len);
void   arena_free(Arena *a);
