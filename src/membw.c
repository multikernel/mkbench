#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAXTHREADS 256

enum kind { K_READ, K_WRITE, K_COPY, K_TRIAD };

static volatile uint64_t sink;
static pthread_barrier_t barrier;
static _Atomic int stop_flag;

struct worker {
	pthread_t tid;
	int cpu;
	int node;
	size_t n;         /* doubles per array */
	enum kind kind;
	double gbps;
};

static void pass_read(const double *a, size_t n)
{
	const uint64_t *p = (const uint64_t *)a;
	uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;

	for (size_t i = 0; i + 4 <= n; i += 4) {
		s0 ^= p[i];
		s1 ^= p[i + 1];
		s2 ^= p[i + 2];
		s3 ^= p[i + 3];
	}
	sink = s0 ^ s1 ^ s2 ^ s3;
}

/* read and write touch one array, copy two, triad three. Allocating all three
 * regardless wasted up to 3x the memory, which on a many-core machine with
 * modest RAM is the difference between running and being OOM-killed. */
static int arrays_for(enum kind kind)
{
	switch (kind) {
	case K_READ:
	case K_WRITE: return 1;
	case K_COPY:  return 2;
	default:      return 3;
	}
}

static void *worker_fn(void *arg)
{
	struct worker *w = arg;
	size_t n = w->n, bytes = n * 8;
	double *a = NULL, *b = NULL, *c = NULL;
	int narrays = arrays_for(w->kind);
	uint64_t passes = 0;
	double t0, t1, bytes_per_pass;

	pin_to_cpu(w->cpu);
	a = alloc_pages_on(bytes, w->node);
	touch_pages(a, bytes);
	if (narrays > 1) {
		b = alloc_pages_on(bytes, w->node);
		touch_pages(b, bytes);
	}
	if (narrays > 2) {
		c = alloc_pages_on(bytes, w->node);
		touch_pages(c, bytes);
	}

	switch (w->kind) {
	case K_READ:  bytes_per_pass = n * 8.0; break;
	case K_WRITE: bytes_per_pass = n * 8.0; break;
	case K_COPY:  bytes_per_pass = n * 16.0; break;
	default:      bytes_per_pass = n * 24.0; break;
	}

	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	do {
		switch (w->kind) {
		case K_READ:
			pass_read(a, n);
			break;
		case K_WRITE:
			for (size_t i = 0; i < n; i++)
				a[i] = 3.0;
			break;
		case K_COPY:
			for (size_t i = 0; i < n; i++)
				a[i] = b[i];
			break;
		case K_TRIAD:
			for (size_t i = 0; i < n; i++)
				a[i] = b[i] + 3.0 * c[i];
			break;
		}
		passes++;
	} while (!atomic_load_explicit(&stop_flag, memory_order_relaxed));
	t1 = now_sec();
	w->gbps = passes * bytes_per_pass / (t1 - t0) / 1e9;
	munmap(a, bytes);
	if (b)
		munmap(b, bytes);
	if (c)
		munmap(c, bytes);
	return NULL;
}

int membw_main(int argc, char **argv)
{
	static struct worker workers[MAXTHREADS];
	int cpus[MAXTHREADS], ncpus = 1, node = 0, repeats = 5, opt;
	long size = 128L << 20;
	double window = 0.5;
	enum kind kind = K_TRIAD;
	const char *kname = "triad", *label = "-";
	char variant[64];

	cpus[0] = 0;
	while ((opt = getopt(argc, argv, "c:m:s:k:r:w:l:")) != -1) {
		switch (opt) {
		case 'c':
			ncpus = parse_cpulist(optarg, cpus, MAXTHREADS);
			if (ncpus <= 0)
				die("bad cpu list '%s'", optarg);
			break;
		case 'm': node = atoi(optarg); break;
		case 's': size = parse_size(optarg); break;
		case 'k':
			kname = optarg;
			if (!strcmp(optarg, "read"))
				kind = K_READ;
			else if (!strcmp(optarg, "write"))
				kind = K_WRITE;
			else if (!strcmp(optarg, "copy"))
				kind = K_COPY;
			else if (!strcmp(optarg, "triad"))
				kind = K_TRIAD;
			else
				die("bad kind '%s'", optarg);
			break;
		case 'r': repeats = atoi(optarg); break;
		case 'w': window = atof(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	/* Footprint grows with thread count, so a size that is fine on one core
	 * can OOM the machine on all of them. Shrink to fit rather than failing
	 * a run several minutes in; the size actually used is recorded in every
	 * CSV row, so a clamped run stays self-describing. */
	{
		long arrays = arrays_for(kind);
		long avail = mem_available_bytes();
		long budget = avail / 2;
		long want = (long)ncpus * arrays * size;

		if (avail > 0 && want > budget) {
			long fit = budget / (ncpus * arrays);

			fit &= ~((1L << 20) - 1);
			if (fit < (1L << 20))
				fit = 1L << 20;
			fprintf(stderr,
				"WARNING: %s on %d threads wants %ld MB but only %ld MB is available; "
				"reducing -s from %ld MB to %ld MB per array\n",
				kname, ncpus, want >> 20, avail >> 20,
				size >> 20, fit >> 20);
			if (fit < (8L << 20))
				fprintf(stderr,
					"WARNING: %ld MB per thread may fit in cache; treat this bandwidth as an upper bound\n",
					fit >> 20);
			size = fit;
		}
	}

	snprintf(variant, sizeof(variant), "%s-%dt", kname, ncpus);

	for (int r = 0; r < repeats; r++) {
		double total = 0;
		struct timespec ts = {
			.tv_sec = (time_t)window,
			.tv_nsec = (long)((window - (time_t)window) * 1e9),
		};

		atomic_store(&stop_flag, 0);
		pthread_barrier_init(&barrier, NULL, ncpus + 1);
		for (int i = 0; i < ncpus; i++) {
			workers[i] = (struct worker){
				.cpu = cpus[i], .node = node,
				.n = (size_t)size / 8, .kind = kind,
			};
			if (pthread_create(&workers[i].tid, NULL, worker_fn,
					   &workers[i]))
				die("pthread_create");
		}
		pthread_barrier_wait(&barrier);
		nanosleep(&ts, NULL);
		atomic_store(&stop_flag, 1);
		for (int i = 0; i < ncpus; i++) {
			pthread_join(workers[i].tid, NULL);
			total += workers[i].gbps;
		}
		pthread_barrier_destroy(&barrier);
		csv_row("membw", variant, label, cpus[0], -1, node, size, r,
			"bw", total, "GB/s");
	}
	return 0;
}
