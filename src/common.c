#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* From <numaif.h>, which ships with libnuma-dev; defined here to avoid the
 * dependency. */
#define MPOL_BIND 2
#define MPOL_INTERLEAVE 3

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

int cacheline_size(void)
{
	static int cached;
	FILE *f;
	int v = 0;

	if (cached)
		return cached;
	f = fopen("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
		  "r");
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = 0;
		fclose(f);
	}
	if (v <= 0)
		v = 64;
	if (v > CACHELINE_MAX) {
		fprintf(stderr,
			"WARNING: cache line is %d bytes but padding assumes at most %d; "
			"raise CACHELINE_MAX or false-sharing results will be wrong\n",
			v, CACHELINE_MAX);
		v = CACHELINE_MAX;
	}
	cached = v;
	return cached;
}

size_t page_size(void)
{
	static size_t cached;

	if (!cached)
		cached = (size_t)sysconf(_SC_PAGESIZE);
	return cached;
}

double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		die("pin to cpu %d: %s", cpu, strerror(errno));
}

static int parse_ranges(const char *s, int *out, int max)
{
	int n = 0;

	while (*s && *s != '\n') {
		char *end;
		long a = strtol(s, &end, 10);
		long b = a;

		if (end == s)
			return -1;
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s)
				return -1;
		}
		for (long v = a; v <= b; v++) {
			if (n >= max)
				return -1;
			out[n++] = (int)v;
		}
		s = end;
		if (*s == ',')
			s++;
	}
	return n;
}

int parse_cpulist(const char *s, int *out, int max)
{
	return parse_ranges(s, out, max);
}

/* A kernel without CONFIG_NUMA exposes no node directory and answers mbind
 * and move_pages with ENOSYS. There is exactly one memory pool on such a
 * machine, so the default policy is already the right one. */
int numa_available(void)
{
	static int cached = -1;

	if (cached < 0)
		cached = access("/sys/devices/system/node/online", R_OK) == 0;
	return cached;
}

static unsigned long online_node_mask(void)
{
	char buf[256];
	int nodes[64];
	unsigned long mask = 0;
	int n;
	FILE *f = fopen("/sys/devices/system/node/online", "r");

	if (!f || !fgets(buf, sizeof(buf), f))
		die("read node online mask: %s", strerror(errno));
	fclose(f);
	n = parse_ranges(buf, nodes, 64);
	if (n <= 0)
		die("parse node online mask '%s'", buf);
	for (int i = 0; i < n; i++)
		mask |= 1UL << nodes[i];
	return mask;
}

void *alloc_pages_on(size_t sz, int node)
{
	void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		die("mmap %zu bytes: %s", sz, strerror(errno));

	if (!numa_available()) {
		if (node > 0)
			die("node %d requested but this kernel has no NUMA support",
			    node);
		return p;
	}

	if (node == -2) {
		unsigned long mask = online_node_mask();

		if (syscall(SYS_mbind, p, sz, MPOL_INTERLEAVE, &mask,
			    sizeof(mask) * 8, 0))
			die("mbind interleave: %s", strerror(errno));
	} else if (node >= 0) {
		unsigned long mask;

		if (node >= 64)
			die("node %d out of range", node);
		mask = 1UL << node;
		if (syscall(SYS_mbind, p, sz, MPOL_BIND, &mask,
			    sizeof(mask) * 8, 0))
			die("mbind node %d: %s", node, strerror(errno));
	}
	return p;
}

void touch_pages(void *p, size_t sz)
{
	memset(p, 1, sz);
}

int page_node(void *p)
{
	int status = -ENOENT;
	void *pages[1] = { p };

	if (syscall(SYS_move_pages, 0, 1UL, pages, NULL, &status, 0))
		return -1;
	return status;
}

long parse_size(const char *s)
{
	char *end;
	long v = strtol(s, &end, 10);

	switch (*end) {
	case 'k': case 'K': v <<= 10; end++; break;
	case 'm': case 'M': v <<= 20; end++; break;
	case 'g': case 'G': v <<= 30; end++; break;
	}
	if (v <= 0 || *end != '\0')
		die("bad size '%s'", s);
	return v;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

double median_of(double *v, int n)
{
	qsort(v, n, sizeof(*v), cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

uint64_t xorshift64(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

void csv_header(void)
{
	puts("test,variant,tier,cpu_a,cpu_b,mem_node,size,repeat,metric,value,unit");
}

void csv_row(const char *test, const char *variant, const char *tier,
	     int cpu_a, int cpu_b, int mem_node, long size,
	     int repeat, const char *metric, double value, const char *unit)
{
	printf("%s,%s,%s,%d,%d,%d,%ld,%d,%s,%.3f,%s\n",
	       test, variant, tier, cpu_a, cpu_b, mem_node, size, repeat,
	       metric, value, unit);
	fflush(stdout);
}

long futex_wait_u32(_Atomic uint32_t *addr, uint32_t val)
{
	return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

long futex_wake_u32(_Atomic uint32_t *addr, int n)
{
	return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0);
}
