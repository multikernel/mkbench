#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MAXTHREADS 256

static _Atomic uint64_t counter __attribute__((aligned(CACHELINE_MAX)));
static _Atomic int stop_flag __attribute__((aligned(CACHELINE_MAX)));
static pthread_barrier_t barrier;

struct worker {
	pthread_t tid;
	int cpu;
};

static void *worker_fn(void *arg)
{
	struct worker *w = arg;

	pin_to_cpu(w->cpu);
	pthread_barrier_wait(&barrier);
	while (!atomic_load_explicit(&stop_flag, memory_order_relaxed))
		for (int i = 0; i < 256; i++)
			atomic_fetch_add_explicit(&counter, 1,
						  memory_order_relaxed);
	return NULL;
}

int atomics_main(int argc, char **argv)
{
	static struct worker workers[MAXTHREADS];
	int cpus[MAXTHREADS], ncpus = 1, repeats = 5, window_ms = 500, opt;
	const char *label = "-";
	char variant[32];

	cpus[0] = 0;
	while ((opt = getopt(argc, argv, "c:n:r:l:")) != -1) {
		switch (opt) {
		case 'c':
			ncpus = parse_cpulist(optarg, cpus, MAXTHREADS);
			if (ncpus <= 0)
				die("bad cpu list '%s'", optarg);
			break;
		case 'n': window_ms = atoi(optarg); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	snprintf(variant, sizeof(variant), "%dt", ncpus);

	for (int r = 0; r < repeats; r++) {
		struct timespec ts = {
			.tv_sec = window_ms / 1000,
			.tv_nsec = (long)(window_ms % 1000) * 1000000L,
		};
		double t0, t1, rate;

		atomic_store(&counter, 0);
		atomic_store(&stop_flag, 0);
		pthread_barrier_init(&barrier, NULL, ncpus + 1);
		for (int i = 0; i < ncpus; i++) {
			workers[i].cpu = cpus[i];
			if (pthread_create(&workers[i].tid, NULL, worker_fn,
					   &workers[i]))
				die("pthread_create");
		}
		pthread_barrier_wait(&barrier);
		t0 = now_sec();
		nanosleep(&ts, NULL);
		atomic_store(&stop_flag, 1);
		for (int i = 0; i < ncpus; i++)
			pthread_join(workers[i].tid, NULL);
		t1 = now_sec();
		pthread_barrier_destroy(&barrier);

		rate = (double)atomic_load(&counter) / (t1 - t0);
		csv_row("atomics", variant, label, cpus[0], -1, -1, 0, r,
			"mops", rate / 1e6, "Mops");
		csv_row("atomics", variant, label, cpus[0], -1, -1, 0, r,
			"nsop", 1e9 * ncpus / rate, "ns");
	}
	return 0;
}
