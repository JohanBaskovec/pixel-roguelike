#pragma once

#include "core/common.h"

// A small fork-join thread pool on SDL threads.
//
// jobs_run() calls fn(user, i) for every i in [0, count), spread over the pool's threads (the
// calling thread works too), and returns when all calls have finished. The order in which jobs
// run is unspecified, so jobs must not depend on each other. With a NULL pool, or a pool of one
// thread, jobs run on the caller in index order.
typedef struct JobPool JobPool;
typedef void (*JobFn)(void *user, int index);

// `threads` is the total number of threads, including the caller. <= 0 means one per logical
// CPU core. Returns NULL on failure (callers can then run without a pool).
JobPool *jobs_create(int threads);
void jobs_destroy(JobPool *pool);

int jobs_thread_count(const JobPool *pool);

void jobs_run(JobPool *pool, int count, JobFn fn, void *user);
