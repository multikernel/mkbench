#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Futex ping-pong where both sides actually sleep each round, so every
 * transition pays the full wake path: futex syscall, scheduler wakeup and,
 * cross-CPU, the IPI to the target. This is the userspace proxy for a
 * multikernel doorbell. */

static struct {
	_Atomic uint32_t seq;
	char pad[CACHELINE_MAX - 4];
} fa __attribute__((aligned(CACHELINE_MAX))), fb __attribute__((aligned(CACHELINE_MAX)));

static pthread_barrier_t barrier;

static void wait_for(_Atomic uint32_t *f, uint32_t want)
{
	uint32_t v;

	while ((v = atomic_load_explicit(f, memory_order_acquire)) != want)
		futex_wait_u32(f, v);
}

struct ctx {
	int cpu;
	uint32_t rounds;
};

static void *peer_fn(void *arg)
{
	struct ctx *c = arg;

	pin_to_cpu(c->cpu);
	pthread_barrier_wait(&barrier);
	for (uint32_t r = 1; r <= c->rounds; r++) {
		wait_for(&fb.seq, r);
		atomic_store_explicit(&fa.seq, r, memory_order_release);
		futex_wake_u32(&fa.seq, 1);
	}
	return NULL;
}

static double run_rounds(int cpu_a, int cpu_b, uint32_t rounds)
{
	struct ctx c = { .cpu = cpu_b, .rounds = rounds };
	pthread_t tid;
	double t0, t1;

	atomic_store(&fa.seq, 0);
	atomic_store(&fb.seq, 0);
	pthread_barrier_init(&barrier, NULL, 2);
	if (pthread_create(&tid, NULL, peer_fn, &c))
		die("pthread_create");
	pin_to_cpu(cpu_a);
	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	for (uint32_t r = 1; r <= rounds; r++) {
		atomic_store_explicit(&fb.seq, r, memory_order_release);
		futex_wake_u32(&fb.seq, 1);
		wait_for(&fa.seq, r);
	}
	t1 = now_sec();
	pthread_join(tid, NULL);
	pthread_barrier_destroy(&barrier);
	return (t1 - t0) * 1e9 / (double)rounds / 2.0;
}

int wakeup_main(int argc, char **argv)
{
	int cpu_a = 0, cpu_b = 1, repeats = 5, opt;
	uint32_t rounds = 20000;
	const char *label = "-";

	while ((opt = getopt(argc, argv, "a:b:n:r:l:")) != -1) {
		switch (opt) {
		case 'a': cpu_a = atoi(optarg); break;
		case 'b': cpu_b = atoi(optarg); break;
		case 'n': rounds = (uint32_t)strtoul(optarg, NULL, 10); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}

	run_rounds(cpu_a, cpu_b, rounds / 10);  /* warmup */
	for (int r = 0; r < repeats; r++)
		csv_row("wakeup", "futex", label, cpu_a, cpu_b, -1, 0, r,
			"wake", run_rounds(cpu_a, cpu_b, rounds), "ns");
	return 0;
}
