#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile uint64_t sink;

/* Random cyclic permutation over cache lines (Sattolo's algorithm) so every
 * load depends on the previous one and the prefetcher sees no pattern. */
static void build_chase(char *buf, size_t lines, size_t stride)
{
	uint64_t rng = 0x9e3779b97f4a7c15ULL;
	size_t *order = malloc(lines * sizeof(*order));

	if (!order)
		die("malloc chase order");
	for (size_t i = 0; i < lines; i++)
		order[i] = i;
	for (size_t i = lines - 1; i > 0; i--) {
		size_t j = xorshift64(&rng) % i;
		size_t t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (size_t i = 0; i < lines; i++) {
		size_t next = order[(i + 1) % lines];

		*(uint64_t *)(buf + order[i] * stride) = next * stride;
	}
	free(order);
}

static double chase(const char *buf, uint64_t iters)
{
	uint64_t off = 0;
	double t0, t1;

	t0 = now_sec();
	for (uint64_t i = 0; i < iters; i++)
		off = *(const uint64_t *)(buf + off);
	t1 = now_sec();
	sink = off;
	return (t1 - t0) * 1e9 / (double)iters;
}

int memlat_main(int argc, char **argv)
{
	int cpu = 0, node = 0, repeats = 5, opt;
	long size = 512L << 20;
	uint64_t iters = 0;
	const char *label = "-";
	char *buf;
	size_t lines, stride;

	while ((opt = getopt(argc, argv, "c:m:s:n:r:l:")) != -1) {
		switch (opt) {
		case 'c': cpu = atoi(optarg); break;
		case 'm': node = atoi(optarg); break;
		case 's': size = parse_size(optarg); break;
		case 'n': iters = strtoull(optarg, NULL, 10); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	stride = (size_t)cacheline_size();
	lines = (size_t)size / stride;
	if (lines < 2)
		die("size too small");

	pin_to_cpu(cpu);
	buf = alloc_pages_on((size_t)size, node);
	touch_pages(buf, (size_t)size);
	build_chase(buf, lines, stride);

	/* Warmup: one full traversal, then calibrate iters to ~0.5 s. */
	chase(buf, lines);
	if (!iters) {
		double ns = chase(buf, 1 << 15);

		iters = (uint64_t)(0.5e9 / ns);
		if (iters < (1 << 20))
			iters = 1 << 20;
	}

	for (int r = 0; r < repeats; r++)
		csv_row("memlat", "chase", label, cpu, -1, node, size, r,
			"lat", chase(buf, iters), "ns");
	return 0;
}
