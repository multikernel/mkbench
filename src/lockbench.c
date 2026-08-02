#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXTHREADS 256

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_spinlock_t spin;
static uint64_t shared_counter;
static _Atomic int stop_flag __attribute__((aligned(CACHELINE)));
static pthread_barrier_t barrier;
static int use_spin;

struct worker {
	pthread_t tid;
	int cpu;
};

static void *worker_fn(void *arg)
{
	struct worker *w = arg;

	pin_to_cpu(w->cpu);
	pthread_barrier_wait(&barrier);
	while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
		if (use_spin) {
			pthread_spin_lock(&spin);
			shared_counter++;
			pthread_spin_unlock(&spin);
		} else {
			pthread_mutex_lock(&mutex);
			shared_counter++;
			pthread_mutex_unlock(&mutex);
		}
	}
	return NULL;
}

int lock_main(int argc, char **argv)
{
	static struct worker workers[MAXTHREADS];
	int cpus[MAXTHREADS], ncpus = 1, repeats = 5, window_ms = 500, opt;
	const char *kind = "mutex", *label = "-";
	char variant[48];

	cpus[0] = 0;
	while ((opt = getopt(argc, argv, "c:k:n:r:l:")) != -1) {
		switch (opt) {
		case 'c':
			ncpus = parse_cpulist(optarg, cpus, MAXTHREADS);
			if (ncpus <= 0)
				die("bad cpu list '%s'", optarg);
			break;
		case 'k': kind = optarg; break;
		case 'n': window_ms = atoi(optarg); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	if (!strcmp(kind, "spin"))
		use_spin = 1;
	else if (strcmp(kind, "mutex"))
		die("bad kind '%s'", kind);
	snprintf(variant, sizeof(variant), "%s-%dt", kind, ncpus);
	pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);

	for (int r = 0; r < repeats; r++) {
		struct timespec ts = {
			.tv_sec = window_ms / 1000,
			.tv_nsec = (long)(window_ms % 1000) * 1000000L,
		};
		double t0, t1, rate;

		shared_counter = 0;
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

		rate = (double)shared_counter / (t1 - t0);
		csv_row("lock", variant, label, cpus[0],
			ncpus > 1 ? cpus[ncpus - 1] : -1, -1, 0, r,
			"mops", rate / 1e6, "Mops");
		csv_row("lock", variant, label, cpus[0],
			ncpus > 1 ? cpus[ncpus - 1] : -1, -1, 0, r,
			"nsacq", 1e9 * ncpus / rate, "ns");
	}
	return 0;
}
