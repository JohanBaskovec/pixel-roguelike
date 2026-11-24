#include "core/jobs.h"

#include "core/log.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <stdlib.h>

#define MAX_THREADS 64
// After a batch, a worker (and the caller, waiting for the last jobs) spins this long watching for
// the next before sleeping on the condition variable. A tick runs about 8 batches microseconds
// apart; waking a sleeping thread costs tens of microseconds each time (about 55 us per batch on
// a 4-core VM). Between ticks, after this, they sleep.
#define SPIN_SECONDS 0.0002

struct JobPool {
    int threads; // including the caller
    SDL_Thread *workers[MAX_THREADS];
    SDL_Mutex *mutex;
    SDL_Condition *work_cv; // a new batch is available (or quit)
    SDL_Condition *done_cv; // a worker went idle

    // Current batch. Written under the mutex while no worker is busy.
    JobFn fn;
    void *user;
    int count;
    SDL_AtomicInt next; // next job index to claim

    uint64_t generation; // incremented per batch (under mutex)
    int busy;            // workers inside a batch (under mutex)
    bool quit;
    // Copies of generation (low 32 bits) and busy, written under the mutex, read without it while
    // spinning; the decision is always taken again under the mutex.
    SDL_AtomicInt generation_seen;
    SDL_AtomicInt busy_seen;
};

// Spins until the atomic differs from `value` (differ) or equals it (!differ), or SPIN_SECONDS
// pass. Returns true if it happened.
static bool spin_until(SDL_AtomicInt *a, int value, bool differ) {
    uint64_t start = SDL_GetPerformanceCounter();
    uint64_t limit = (uint64_t)(SPIN_SECONDS * (double)SDL_GetPerformanceFrequency());
    for (int k = 0;; k++) {
        if ((SDL_GetAtomicInt(a) != value) == differ) {
            return true;
        }
        SDL_CPUPauseInstruction();
        if ((k & 63) == 63 && SDL_GetPerformanceCounter() - start > limit) {
            return false;
        }
    }
}

static void run_batch(JobPool *pool) {
    for (;;) {
        int i = SDL_AddAtomicInt(&pool->next, 1);
        if (i >= pool->count) {
            return;
        }
        pool->fn(pool->user, i);
    }
}

static int worker_main(void *arg) {
    JobPool *pool = arg;
    uint64_t seen = 0;
    SDL_LockMutex(pool->mutex);
    for (;;) {
        while (!pool->quit && pool->generation == seen) {
            SDL_WaitCondition(pool->work_cv, pool->mutex);
        }
        if (pool->quit) {
            break;
        }
        seen = pool->generation;
        pool->busy++;
        SDL_SetAtomicInt(&pool->busy_seen, pool->busy);
        SDL_UnlockMutex(pool->mutex);

        run_batch(pool);

        SDL_LockMutex(pool->mutex);
        pool->busy--;
        SDL_SetAtomicInt(&pool->busy_seen, pool->busy);
        if (pool->busy == 0) {
            SDL_BroadcastCondition(pool->done_cv);
        }
        // Watch for the next batch for a moment before sleeping (SPIN_SECONDS).
        SDL_UnlockMutex(pool->mutex);
        spin_until(&pool->generation_seen, (int)(uint32_t)seen, true);
        SDL_LockMutex(pool->mutex);
    }
    SDL_UnlockMutex(pool->mutex);
    return 0;
}

JobPool *jobs_create(int threads) {
    if (threads <= 0) {
        threads = SDL_GetNumLogicalCPUCores();
    }
    threads = PR_CLAMP(threads, 1, MAX_THREADS);

    JobPool *pool = calloc(1, sizeof(JobPool));
    if (!pool) {
        return NULL;
    }
    pool->threads = threads;
    pool->mutex = SDL_CreateMutex();
    pool->work_cv = SDL_CreateCondition();
    pool->done_cv = SDL_CreateCondition();
    if (!pool->mutex || !pool->work_cv || !pool->done_cv) {
        pr_log_error("jobs_create: %s", SDL_GetError());
        jobs_destroy(pool);
        return NULL;
    }
    for (int i = 1; i < threads; i++) {
        pool->workers[i] = SDL_CreateThread(worker_main, "pr-worker", pool);
        if (!pool->workers[i]) {
            pr_log_error("jobs_create: thread %d: %s", i, SDL_GetError());
            pool->threads = i; // run with the threads we got
            break;
        }
    }
    return pool;
}

void jobs_destroy(JobPool *pool) {
    if (!pool) {
        return;
    }
    if (pool->mutex) {
        SDL_LockMutex(pool->mutex);
        pool->quit = true;
        SDL_BroadcastCondition(pool->work_cv);
        SDL_UnlockMutex(pool->mutex);
    }
    for (int i = 1; i < MAX_THREADS; i++) {
        if (pool->workers[i]) {
            SDL_WaitThread(pool->workers[i], NULL);
        }
    }
    SDL_DestroyCondition(pool->work_cv);
    SDL_DestroyCondition(pool->done_cv);
    SDL_DestroyMutex(pool->mutex);
    free(pool);
}

int jobs_thread_count(const JobPool *pool) {
    return pool ? pool->threads : 1;
}

void jobs_run(JobPool *pool, int count, JobFn fn, void *user) {
    if (count <= 0) {
        return;
    }
    if (!pool || pool->threads <= 1 || count == 1) {
        for (int i = 0; i < count; i++) {
            fn(user, i);
        }
        return;
    }

    SDL_LockMutex(pool->mutex);
    while (pool->busy > 0) { // a late worker may still be leaving the previous batch
        SDL_WaitCondition(pool->done_cv, pool->mutex);
    }
    pool->fn = fn;
    pool->user = user;
    pool->count = count;
    SDL_SetAtomicInt(&pool->next, 0);
    pool->generation++;
    SDL_SetAtomicInt(&pool->generation_seen, (int)(uint32_t)pool->generation);
    SDL_BroadcastCondition(pool->work_cv);
    SDL_UnlockMutex(pool->mutex);

    run_batch(pool); // the caller works too

    // All jobs are claimed. Wait for the workers still running one (spinning first).
    spin_until(&pool->busy_seen, 0, false);
    SDL_LockMutex(pool->mutex);
    while (pool->busy > 0) {
        SDL_WaitCondition(pool->done_cv, pool->mutex);
    }
    SDL_UnlockMutex(pool->mutex);
}
