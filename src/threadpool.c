#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_CAP 4096

typedef struct Task {
    task_fn  fn;
    void    *arg;
} Task;

struct ThreadPool {
    pthread_t       *threads;
    int              nthreads;

    Task             queue[QUEUE_CAP];
    int              head, tail, count;

    pthread_mutex_t  mu;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
    pthread_cond_t   done_cond;

    int              active;   /* tasks in-flight */
    int              shutdown;
};

static void *worker(void *arg) {
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->mu);
        while (tp->count == 0 && !tp->shutdown)
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        if (tp->shutdown && tp->count == 0) {
            pthread_mutex_unlock(&tp->mu);
            return NULL;
        }
        Task t = tp->queue[tp->head];
        tp->head = (tp->head + 1) % QUEUE_CAP;
        tp->count--;
        tp->active++;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mu);

        t.fn(t.arg);

        pthread_mutex_lock(&tp->mu);
        tp->active--;
        if (tp->active == 0 && tp->count == 0)
            pthread_cond_broadcast(&tp->done_cond);
        pthread_mutex_unlock(&tp->mu);
    }
}

ThreadPool *threadpool_create(int nthreads) {
    ThreadPool *tp = calloc(1, sizeof(*tp));
    tp->nthreads = nthreads;
    tp->threads  = malloc(nthreads * sizeof(pthread_t));
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);
    pthread_cond_init(&tp->done_cond, NULL);
    for (int i = 0; i < nthreads; i++)
        pthread_create(&tp->threads[i], NULL, worker, tp);
    return tp;
}

void threadpool_submit(ThreadPool *tp, task_fn fn, void *arg) {
    pthread_mutex_lock(&tp->mu);
    while (tp->count == QUEUE_CAP)
        pthread_cond_wait(&tp->not_full, &tp->mu);
    tp->queue[tp->tail].fn  = fn;
    tp->queue[tp->tail].arg = arg;
    tp->tail = (tp->tail + 1) % QUEUE_CAP;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
}

void threadpool_wait(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    while (tp->active > 0 || tp->count > 0)
        pthread_cond_wait(&tp->done_cond, &tp->mu);
    pthread_mutex_unlock(&tp->mu);
}

void threadpool_destroy(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    for (int i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
    pthread_cond_destroy(&tp->done_cond);
    free(tp->threads);
    free(tp);
}
