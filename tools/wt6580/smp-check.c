/* SPDX-License-Identifier: GPL-2.0-only */
/* On-device MT6580 test: affinity, parallel execution and cache coherence. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define CPUS 4
#define INCREMENTS 100000
#define ROUNDS 100000000

static pthread_barrier_t start;
static atomic_uint shared_count;

struct result {
	int cpu, error, first_cpu, last_cpu;
	uint32_t hash;
	double wall, runtime;
};

static double seconds(clockid_t clock)
{
	struct timespec ts;

	clock_gettime(clock, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int check_clock_sync(void)
{
	cpu_set_t original, set;
	double previous, now, max_step = 0;
	int i, failed = 0, backward = 0;

	if (sched_getaffinity(0, sizeof(original), &original))
		return 1;
	previous = seconds(CLOCK_MONOTONIC);
	for (i = 0; i < CPUS * 100; i++) {
		CPU_ZERO(&set);
		CPU_SET(i % CPUS, &set);
		if (sched_setaffinity(0, sizeof(set), &set)) {
			perror("clock sync affinity");
			failed = 1;
			break;
		}
		now = seconds(CLOCK_MONOTONIC);
		backward += now < previous;
		if (now - previous > max_step)
			max_step = now - previous;
		previous = now;
	}
	failed |= sched_setaffinity(0, sizeof(original), &original) != 0;
	printf("cross-CPU clock: migrations=%d backwards=%d max_step=%.6fs\n",
	       i, backward, max_step);
	return failed || backward;
}

static void *worker(void *arg)
{
	struct result *r = arg;
	cpu_set_t set;
	double wall, runtime;
	uint32_t hash = 0x65809161;
	unsigned int i;

	CPU_ZERO(&set);
	CPU_SET(r->cpu, &set);
	r->error = sched_setaffinity(0, sizeof(set), &set) ? errno : 0;
	/* All workers must reach the barrier, including affinity failures. */
	pthread_barrier_wait(&start);
	if (r->error)
		return NULL;
	r->first_cpu = sched_getcpu();
	wall = seconds(CLOCK_MONOTONIC);
	runtime = seconds(CLOCK_THREAD_CPUTIME_ID);
	for (i = 0; i < INCREMENTS; i++)
		atomic_fetch_add_explicit(&shared_count, 1, memory_order_relaxed);
	for (i = 0; i < ROUNDS; i++) {
		hash ^= hash << 13;
		hash ^= hash >> 17;
		hash ^= hash << 5;
	}
	r->runtime = seconds(CLOCK_THREAD_CPUTIME_ID) - runtime;
	r->wall = seconds(CLOCK_MONOTONIC) - wall;
	r->last_cpu = sched_getcpu();
	r->hash = hash;
	return NULL;
}

int main(void)
{
	struct result results[CPUS] = { 0 };
	pthread_t threads[CPUS];
	double begin, elapsed, total = 0;
	int i, failed = 0, ret;
	unsigned int count;

	failed = check_clock_sync();
	ret = pthread_barrier_init(&start, NULL, CPUS + 1);
	if (ret)
		return 2;
	for (i = 0; i < CPUS; i++) {
		results[i].cpu = i;
		ret = pthread_create(&threads[i], NULL, worker, &results[i]);
		if (ret) {
			fprintf(stderr, "pthread_create CPU%d: %d\n", i, ret);
			return 2;
		}
	}
	begin = seconds(CLOCK_MONOTONIC);
	pthread_barrier_wait(&start);
	for (i = 0; i < CPUS; i++)
		pthread_join(threads[i], NULL);
	elapsed = seconds(CLOCK_MONOTONIC) - begin;
	for (i = 0; i < CPUS; i++) {
		struct result *r = &results[i];

		printf("CPU%d: affinity_error=%d ran=%d..%d hash=%08x cpu=%.3fs wall=%.3fs\n",
		       i, r->error, r->first_cpu, r->last_cpu, r->hash,
		       r->runtime, r->wall);
		failed |= r->error || r->first_cpu != i || r->last_cpu != i ||
			  r->hash != results[0].hash;
		total += r->runtime;
	}
	count = atomic_load_explicit(&shared_count, memory_order_relaxed);
	failed |= count != CPUS * INCREMENTS;
	printf("coherent_count=%u expected=%u CPU time=%.3fs elapsed=%.3fs parallelism=%.2f\n",
	       count, CPUS * INCREMENTS, total, elapsed, total / elapsed);
	puts(failed ? "FAIL" : "PASS");
	return !!failed;
}
