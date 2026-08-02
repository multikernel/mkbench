#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct line {
	_Atomic uint32_t flag;
	char pad[CACHELINE - 4];
} __attribute__((aligned(CACHELINE)));

/* False-sharing layout: two counters either in one line or padded apart. */
struct counters {
	_Atomic uint64_t a;
	char pad[CACHELINE - 8];   /* used only in padded mode */
	_Atomic uint64_t b;
} __attribute__((aligned(CACHELINE)));

struct shared_pair {
	_Atomic uint64_t a;
	_Atomic uint64_t b;
} __attribute__((aligned(CACHELINE)));

static struct line ping;
static _Atomic int stop_flag __attribute__((aligned(CACHELINE)));
static pthread_barrier_t barrier;

struct ctx {
	int cpu;
	uint64_t iters;
	_Atomic uint64_t *ctr;
	uint64_t count;
};

static void *rtt_peer(void *arg)
{
	struct ctx *c = arg;

	pin_to_cpu(c->cpu);
	pthread_barrier_wait(&barrier);
	for (uint64_t i = 0; i < c->iters; i++) {
		while (atomic_load_explicit(&ping.flag, memory_order_acquire)
		       % 2 == 0)
			cpu_relax();
		atomic_fetch_add_explicit(&ping.flag, 1, memory_order_release);
	}
	return NULL;
}

static double run_rtt(int cpu_a, int cpu_b, uint64_t iters)
{
	struct ctx c = { .cpu = cpu_b, .iters = iters };
	pthread_t tid;
	double t0, t1;

	atomic_store(&ping.flag, 0);
	pthread_barrier_init(&barrier, NULL, 2);
	if (pthread_create(&tid, NULL, rtt_peer, &c))
		die("pthread_create");
	pin_to_cpu(cpu_a);
	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	for (uint64_t i = 0; i < iters; i++) {
		atomic_fetch_add_explicit(&ping.flag, 1, memory_order_release);
		while (atomic_load_explicit(&ping.flag, memory_order_acquire)
		       % 2 == 1)
			cpu_relax();
	}
	t1 = now_sec();
	pthread_join(tid, NULL);
	pthread_barrier_destroy(&barrier);
	return (t1 - t0) * 1e9 / (double)iters;
}

static void *bump_peer(void *arg)
{
	struct ctx *c = arg;

	pin_to_cpu(c->cpu);
	pthread_barrier_wait(&barrier);
	while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
		for (int i = 0; i < 1024; i++)
			atomic_fetch_add_explicit(c->ctr, 1,
						  memory_order_relaxed);
		c->count += 1024;
	}
	return NULL;
}

static double run_false(int cpu_a, int cpu_b, int padded, double window)
{
	static struct counters padded_ctrs;
	static struct shared_pair shared_ctrs;
	struct ctx ca = { .cpu = cpu_a }, cb = { .cpu = cpu_b };
	pthread_t ta, tb;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)(window * 1e9) };
	double t0, t1;

	if (padded) {
		ca.ctr = &padded_ctrs.a;
		cb.ctr = &padded_ctrs.b;
	} else {
		ca.ctr = &shared_ctrs.a;
		cb.ctr = &shared_ctrs.b;
	}
	atomic_store(&stop_flag, 0);
	pthread_barrier_init(&barrier, NULL, 3);
	if (pthread_create(&ta, NULL, bump_peer, &ca) ||
	    pthread_create(&tb, NULL, bump_peer, &cb))
		die("pthread_create");
	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	nanosleep(&ts, NULL);
	atomic_store(&stop_flag, 1);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	t1 = now_sec();
	pthread_barrier_destroy(&barrier);
	return (ca.count + cb.count) / (t1 - t0) / 1e6;
}

int pingpong_main(int argc, char **argv)
{
	int cpu_a = 0, cpu_b = 1, repeats = 5, opt;
	uint64_t iters = 1000000;
	const char *kind = "rtt", *label = "-";

	while ((opt = getopt(argc, argv, "a:b:k:n:r:l:")) != -1) {
		switch (opt) {
		case 'a': cpu_a = atoi(optarg); break;
		case 'b': cpu_b = atoi(optarg); break;
		case 'k': kind = optarg; break;
		case 'n': iters = strtoull(optarg, NULL, 10); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	if (cpu_a == cpu_b)
		die("pingpong needs two distinct CPUs (spinning would deadlock the scheduler slice)");

	for (int r = 0; r < repeats; r++) {
		if (!strcmp(kind, "rtt")) {
			run_rtt(cpu_a, cpu_b, iters / 10);  /* warmup */
			csv_row("pingpong", kind, label, cpu_a, cpu_b, -1, 0,
				r, "rtt", run_rtt(cpu_a, cpu_b, iters), "ns");
		} else if (!strcmp(kind, "false-shared")) {
			csv_row("pingpong", kind, label, cpu_a, cpu_b, -1, 0,
				r, "ops", run_false(cpu_a, cpu_b, 0, 0.5),
				"Mops");
		} else if (!strcmp(kind, "false-padded")) {
			csv_row("pingpong", kind, label, cpu_a, cpu_b, -1, 0,
				r, "ops", run_false(cpu_a, cpu_b, 1, 0.5),
				"Mops");
		} else {
			die("bad kind '%s'", kind);
		}
	}
	return 0;
}
