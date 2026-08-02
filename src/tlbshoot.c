#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* TLB shootdown: an unmap or permission tightening must IPI every CPU in the
 * process's mm_cpumask and block until all of them acknowledge. The initiator
 * therefore pays for the slowest participant, which is what makes the cost
 * grow with participant count and jump when participants sit across the
 * interconnect. Participants only have to be running this mm to be in the
 * mask, so they read an unrelated region rather than the target; what is
 * being measured is the IPI broadcast and ack wait, not their local flush. */

#define MAXPARTS 512
#define TOUCH_BYTES (32UL << 20)
#define WORK_LINES 64
#define SLICE_SEC 0.020

/* Round latencies go into a small per-phase histogram rather than a sample
 * array: a growing array is megabytes of streaming stores that only the
 * shootdown phase would pay, which registers as a slowdown of its own, and it
 * silently truncates once full. Two histograms indexed by phase keep the work
 * identical on both sides. */
#define HIST_BUCKETS 1024
#define HIST_NS 32.0

enum op { OP_MPROTECT, OP_DONTNEED };

static _Atomic int stop_flag __attribute__((aligned(CACHELINE_MAX)));
static _Atomic int sampling __attribute__((aligned(CACHELINE_MAX)));
static pthread_barrier_t barrier;
static char *touch_region;
static volatile uint64_t sink;

struct part {
	pthread_t tid;
	int cpu;
	int jitter;
	uint32_t hist[2][HIST_BUCKETS];
	_Atomic uint64_t rounds;
};

static void *part_fn(void *arg)
{
	struct part *p = arg;
	uint64_t acc = 0;
	size_t off = 0, stride = (size_t)cacheline_size();
	int b;

	pin_to_cpu(p->cpu);
	pthread_barrier_wait(&barrier);
	while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
		/* Timestamp in both phases even though only shootdown-slice
		 * samples are kept: the two clock reads cost a sizeable
		 * fraction of a round, and paying them in just one phase
		 * would show up as a slowdown all by itself. */
		double t0 = p->jitter ? now_sec() : 0.0;
		double el;

		for (int i = 0; i < WORK_LINES; i++) {
			acc += *(volatile uint64_t *)(touch_region + off);
			off += stride;
			if (off >= TOUCH_BYTES)
				off = 0;
		}
		atomic_fetch_add_explicit(&p->rounds, 1, memory_order_relaxed);
		if (!p->jitter)
			continue;
		el = (now_sec() - t0) * 1e9;
		b = (int)(el / HIST_NS);
		if (b >= HIST_BUCKETS)
			b = HIST_BUCKETS - 1;
		p->hist[atomic_load_explicit(&sampling,
					     memory_order_relaxed)][b]++;
	}
	sink = acc;
	return NULL;
}

static uint64_t sum_rounds(struct part *parts, int n)
{
	uint64_t t = 0;

	for (int i = 0; i < n; i++)
		t += atomic_load_explicit(&parts[i].rounds,
					  memory_order_relaxed);
	return t;
}

static void spin_until(double deadline)
{
	while (now_sec() < deadline)
		cpu_relax();
}

static double hist_pct(const uint64_t *h, double pct)
{
	uint64_t total = 0, target, cum = 0;

	for (int i = 0; i < HIST_BUCKETS; i++)
		total += h[i];
	if (!total)
		return 0;
	target = (uint64_t)(total * pct);
	for (int i = 0; i < HIST_BUCKETS; i++) {
		cum += h[i];
		if (cum >= target)
			return (i + 0.5) * HIST_NS;
	}
	return HIST_BUCKETS * HIST_NS;
}

/* Guard pages on both sides keep the target in its own VMA, so flipping
 * protection does not merge or split it and we time the flush, not VMA
 * bookkeeping. */
static char *make_target(size_t len)
{
	size_t pg = page_size();
	size_t total = len + 2 * pg;
	char *base = mmap(NULL, total, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char *target;

	if (base == MAP_FAILED)
		die("mmap target: %s", strerror(errno));
	target = base + pg;
	if (mprotect(target, len, PROT_READ | PROT_WRITE))
		die("mprotect target: %s", strerror(errno));
	touch_pages(target, len);
	return target;
}

static double shoot_once(char *target, size_t len, enum op op)
{
	double t0, t1;

	if (op == OP_MPROTECT) {
		/* Only the tightening direction is timed: relaxing
		 * permissions does not require a synchronous flush. */
		t0 = now_sec();
		if (mprotect(target, len, PROT_READ))
			die("mprotect RO: %s", strerror(errno));
		t1 = now_sec();
		if (mprotect(target, len, PROT_READ | PROT_WRITE))
			die("mprotect RW: %s", strerror(errno));
	} else {
		t0 = now_sec();
		if (madvise(target, len, MADV_DONTNEED))
			die("madvise DONTNEED: %s", strerror(errno));
		t1 = now_sec();
		touch_pages(target, len);   /* refault, else next zap is a no-op */
	}
	return (t1 - t0) * 1e9;
}

static void start_parts(struct part *parts, int n, int *cpus, int jitter)
{
	atomic_store(&stop_flag, 0);
	atomic_store(&sampling, 0);
	pthread_barrier_init(&barrier, NULL, n + 1);
	for (int i = 0; i < n; i++) {
		parts[i].cpu = cpus[i];
		parts[i].jitter = jitter;
		memset(parts[i].hist, 0, sizeof(parts[i].hist));
		atomic_store(&parts[i].rounds, 0);
		if (pthread_create(&parts[i].tid, NULL, part_fn, &parts[i]))
			die("pthread_create");
	}
	pthread_barrier_wait(&barrier);
}

static void stop_parts(struct part *parts, int n)
{
	atomic_store(&stop_flag, 1);
	for (int i = 0; i < n; i++)
		pthread_join(parts[i].tid, NULL);
	pthread_barrier_destroy(&barrier);
}

int tlbshoot_main(int argc, char **argv)
{
	static struct part parts[MAXPARTS];
	int cpu_a = 0, cpus[MAXPARTS], nparts = 0, repeats = 5, opt;
	int npages = 1, jitter = 0, no_shoot = 0, window_ms = 500;
	int interval_us = 0;
	uint64_t iters = 20000;
	enum op op = OP_MPROTECT;
	const char *label = "-", *opname = "mprotect";
	char variant[64];
	size_t len;
	char *target;

	while ((opt = getopt(argc, argv, "a:c:k:o:p:n:r:l:w:i:")) != -1) {
		switch (opt) {
		case 'a': cpu_a = atoi(optarg); break;
		case 'c':
			nparts = parse_cpulist(optarg, cpus, MAXPARTS);
			if (nparts <= 0)
				die("bad cpu list '%s'", optarg);
			break;
		case 'k':
			if (!strcmp(optarg, "jitter")) {
				jitter = 1;
			} else if (!strcmp(optarg, "control")) {
				/* Same structure, zero shootdowns: whatever
				 * slowdown this reports is the noise floor. */
				jitter = 1;
				no_shoot = 1;
			} else if (strcmp(optarg, "lat")) {
				die("bad kind '%s'", optarg);
			}
			break;
		case 'o':
			opname = optarg;
			if (!strcmp(optarg, "mprotect"))
				op = OP_MPROTECT;
			else if (!strcmp(optarg, "dontneed"))
				op = OP_DONTNEED;
			else
				die("bad op '%s'", optarg);
			break;
		case 'p': npages = atoi(optarg); break;
		case 'n': iters = strtoull(optarg, NULL, 10); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		case 'w': window_ms = atoi(optarg); break;
		case 'i': interval_us = atoi(optarg); break;
		default: return 1;
		}
	}
	if (npages < 1)
		die("npages must be >= 1");
	for (int i = 0; i < nparts; i++)
		if (cpus[i] == cpu_a)
			die("initiator cpu %d must not be a participant", cpu_a);
	if (jitter && !nparts)
		die("jitter mode needs participants");

	len = (size_t)npages * page_size();
	if (no_shoot)
		snprintf(variant, sizeof(variant), "%s-%dp-%dt-ctl", opname,
			 npages, nparts);
	else if (jitter && interval_us)
		/* Pacing changes the experiment, so it belongs in the variant:
		 * otherwise a paced and an unpaced run share a row and get
		 * silently averaged together. */
		snprintf(variant, sizeof(variant), "%s-%dp-%dt-i%d", opname,
			 npages, nparts, interval_us);
	else
		snprintf(variant, sizeof(variant), "%s-%dp-%dt", opname,
			 npages, nparts);

	touch_region = alloc_pages_on(TOUCH_BYTES, -1);
	touch_pages(touch_region, TOUCH_BYTES);
	target = make_target(len);
	pin_to_cpu(cpu_a);

	for (int r = 0; r < repeats; r++) {
		if (nparts)
			start_parts(parts, nparts, cpus, jitter);

		if (!jitter) {
			double *s = malloc(iters * sizeof(double));

			if (!s)
				die("malloc samples");
			for (uint64_t i = 0; i < iters / 10; i++)
				shoot_once(target, len, op);
			for (uint64_t i = 0; i < iters; i++)
				s[i] = shoot_once(target, len, op);
			if (nparts)
				stop_parts(parts, nparts);
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "shoot_med",
				median_of(s, (int)iters), "ns");
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "shoot_p99",
				s[iters * 99 / 100], "ns");
			free(s);
		} else {
			/* Alternate short quiet and shootdown slices instead
			 * of running one long phase of each: the participants'
			 * throughput ratio is then immune to monotonic drift
			 * such as frequency ramping, which otherwise makes
			 * whichever phase runs second look faster. */
			double slice = SLICE_SEC;
			double quiet_t = 0, shot_t = 0, next;
			double quiet_rate, shot_rate;
			uint64_t quiet_rounds = 0, shot_rounds = 0, shots = 0;
			uint64_t agg[HIST_BUCKETS] = { 0 };
			double p99;
			int pairs;

			pairs = (int)(window_ms / 1000.0 / (2 * slice));
			if (pairs < 1)
				pairs = 1;
			spin_until(now_sec() + slice);   /* warmup, discarded */

			for (int i = 0; i < pairs; i++) {
				uint64_t r0;
				double a, b;

				atomic_store(&sampling, 0);
				r0 = sum_rounds(parts, nparts);
				a = now_sec();
				spin_until(a + slice);
				b = now_sec();
				quiet_rounds += sum_rounds(parts, nparts) - r0;
				quiet_t += b - a;

				atomic_store(&sampling, 1);
				r0 = sum_rounds(parts, nparts);
				a = now_sec();
				next = a;
				while (now_sec() < a + slice) {
					if (no_shoot) {
						spin_until(a + slice);
						break;
					}
					shoot_once(target, len, op);
					shots++;
					if (!interval_us)
						continue;
					next += interval_us * 1e-6;
					/* Never let pacing overrun the slice,
					 * or the two phases stop being equal
					 * length and drift no longer cancels. */
					spin_until(next < a + slice ? next
								   : a + slice);
				}
				b = now_sec();
				shot_rounds += sum_rounds(parts, nparts) - r0;
				shot_t += b - a;
			}
			quiet_rate = quiet_rounds / quiet_t;
			shot_rate = shot_rounds / shot_t;
			stop_parts(parts, nparts);

			for (int i = 0; i < nparts; i++)
				for (int b = 0; b < HIST_BUCKETS; b++)
					agg[b] += parts[i].hist[1][b];
			p99 = hist_pct(agg, 0.99);

			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "victim_quiet",
				quiet_rate / 1e3, "Kround/s");
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "victim_shot",
				shot_rate / 1e3, "Kround/s");
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "victim_slowdown",
				shot_rate > 0 ? quiet_rate / shot_rate : 0,
				"x");
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "victim_p99", p99, "ns");
			csv_row("tlbshoot", variant, label, cpu_a, -1, -1,
				(long)len, r, "shoot_rate",
				shots / shot_t / 1e3, "Kshot/s");
		}
	}
	return 0;
}
