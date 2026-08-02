#ifndef MKB_COMMON_H
#define MKB_COMMON_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Alignment and padding have to be compile-time, so they use the widest
 * coherency line any supported machine has: 64 on x86 and most ARM servers,
 * 128 on Apple silicon, 256 on A64FX. Over-aligning costs a little memory and
 * keeps isolation correct everywhere. Anything that must match the real line
 * width, such as a traversal stride, calls cacheline_size() instead. */
#define CACHELINE_MAX 256

int cacheline_size(void);
size_t page_size(void);

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

double now_sec(void);
void pin_to_cpu(int cpu);

/* node >= 0: MPOL_BIND to that node; -1: default policy; -2: interleave
 * across all online nodes. Memory is mmap'd and unpopulated; callers must
 * touch it from the right context so pages fault in under the bound policy. */
void *alloc_pages_on(size_t sz, int node);
void touch_pages(void *p, size_t sz);
int page_node(void *p);

long parse_size(const char *s);
int parse_cpulist(const char *s, int *out, int max);
double median_of(double *v, int n);
uint64_t xorshift64(uint64_t *state);

void csv_header(void);
void csv_row(const char *test, const char *variant, const char *tier,
             int cpu_a, int cpu_b, int mem_node, long size,
             int repeat, const char *metric, double value, const char *unit);

long futex_wait_u32(_Atomic uint32_t *addr, uint32_t val);
long futex_wake_u32(_Atomic uint32_t *addr, int n);

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#elif defined(__riscv)
	/* Zihintpause `pause`, emitted as a raw word so it assembles without
	 * requiring toolchain support for the mnemonic. It sits in the FENCE
	 * encoding space with an empty successor set, which older hardware
	 * retires as a no-op. */
	__asm__ __volatile__(".4byte 0x0100000F");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

int check_main(int argc, char **argv);
int memlat_main(int argc, char **argv);
int membw_main(int argc, char **argv);
int pingpong_main(int argc, char **argv);
int atomics_main(int argc, char **argv);
int lock_main(int argc, char **argv);
int ipc_main(int argc, char **argv);
int wakeup_main(int argc, char **argv);
int tlbshoot_main(int argc, char **argv);

#endif
