#pragma once
#include <stddef.h>

typedef void (*task_fn)(void *arg);

typedef struct ThreadPool ThreadPool;

ThreadPool *threadpool_create(int nthreads);
void        threadpool_submit(ThreadPool *tp, task_fn fn, void *arg);
void        threadpool_wait(ThreadPool *tp);
void        threadpool_destroy(ThreadPool *tp);
