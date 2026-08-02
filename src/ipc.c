#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAXMSG 4096
#define RING_SLOTS 1024
#define WARMUP 1000

enum kind { K_RING, K_RINGWAIT, K_PIPE, K_UNIX, K_TCP };

/* Single-slot mailbox: seq parity encodes full/empty, payload on its own
 * lines. In wait mode the sleeper parks on a futex and the writer rings the
 * doorbell with futex_wake, modeling a shared-memory cross-kernel channel. */
struct mbox {
	_Atomic uint32_t seq;
	char pad[CACHELINE - 4];
	char buf[MAXMSG];
} __attribute__((aligned(CACHELINE)));

static struct mbox mb_ab, mb_ba;
static pthread_barrier_t barrier;

static void mbox_xfer(struct mbox *m, char *msg, size_t z, int wait,
		      uint32_t want_parity, int is_send)
{
	uint32_t s;

	while (((s = atomic_load_explicit(&m->seq, memory_order_acquire)) & 1)
	       != want_parity) {
		if (wait)
			futex_wait_u32(&m->seq, s);
		else
			cpu_relax();
	}
	if (is_send)
		memcpy(m->buf, msg, z);
	else
		memcpy(msg, m->buf, z);
	atomic_store_explicit(&m->seq, s + 1, memory_order_release);
	if (wait)
		futex_wake_u32(&m->seq, 1);
}

#define mbox_send(m, msg, z, wait) mbox_xfer(m, msg, z, wait, 0, 1)
#define mbox_recv(m, msg, z, wait) mbox_xfer(m, msg, z, wait, 1, 0)

static void write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);

		if (w <= 0)
			die("write: %s", w ? strerror(errno) : "eof");
		p += w;
		n -= (size_t)w;
	}
}

/* Returns 0 on clean EOF at a message boundary, 1 on a full message. */
static int read_msg(int fd, char *p, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);

		if (r == 0) {
			if (got == 0)
				return 0;
			die("read: short message at eof");
		}
		if (r < 0)
			die("read: %s", strerror(errno));
		got += (size_t)r;
	}
	return 1;
}

/* ---- fd transport setup: A uses [0], B uses [1] of each endpoint ---- */

struct endpoints {
	int rfd[2], wfd[2];
};

static void setup_pipe(struct endpoints *e)
{
	int a2b[2], b2a[2];

	if (pipe(a2b) || pipe(b2a))
		die("pipe: %s", strerror(errno));
	e->rfd[0] = b2a[0]; e->wfd[0] = a2b[1];
	e->rfd[1] = a2b[0]; e->wfd[1] = b2a[1];
}

static void setup_unix(struct endpoints *e)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		die("socketpair: %s", strerror(errno));
	e->rfd[0] = e->wfd[0] = sv[0];
	e->rfd[1] = e->wfd[1] = sv[1];
}

static void setup_tcp(struct endpoints *e)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t alen = sizeof(addr);
	int one = 1;
	int l = socket(AF_INET, SOCK_STREAM, 0);
	int c, s;

	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (l < 0 || bind(l, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(l, 1) || getsockname(l, (struct sockaddr *)&addr, &alen))
		die("tcp listen: %s", strerror(errno));
	c = socket(AF_INET, SOCK_STREAM, 0);
	if (c < 0 || connect(c, (struct sockaddr *)&addr, sizeof(addr)))
		die("tcp connect: %s", strerror(errno));
	s = accept(l, NULL, NULL);
	if (s < 0)
		die("tcp accept: %s", strerror(errno));
	close(l);
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	e->rfd[0] = e->wfd[0] = c;
	e->rfd[1] = e->wfd[1] = s;
}

static void close_endpoint(struct endpoints *e, int side)
{
	close(e->rfd[side]);
	if (e->wfd[side] != e->rfd[side])
		close(e->wfd[side]);
}

/* ---- latency: N timed round trips of z-byte messages ---- */

struct peer_ctx {
	int cpu;
	enum kind kind;
	struct endpoints *e;
	uint64_t iters;
	size_t z;
};

static void *lat_peer(void *arg)
{
	struct peer_ctx *c = arg;
	char msg[MAXMSG];
	int wait = c->kind == K_RINGWAIT;

	pin_to_cpu(c->cpu);
	pthread_barrier_wait(&barrier);
	for (uint64_t i = 0; i < c->iters; i++) {
		if (c->kind == K_RING || c->kind == K_RINGWAIT) {
			mbox_recv(&mb_ab, msg, c->z, wait);
			mbox_send(&mb_ba, msg, c->z, wait);
		} else {
			read_msg(c->e->rfd[1], msg, c->z);
			write_all(c->e->wfd[1], msg, c->z);
		}
	}
	return NULL;
}

static void run_latency(enum kind kind, const char *kname, int cpu_a,
			int cpu_b, size_t z, uint64_t iters, int repeats,
			const char *label)
{
	for (int rep = 0; rep < repeats; rep++) {
		struct endpoints e = { .rfd = { -1, -1 }, .wfd = { -1, -1 } };
		struct peer_ctx c = {
			.cpu = cpu_b, .kind = kind, .e = &e,
			.iters = iters + WARMUP, .z = z,
		};
		double *rtts = malloc(iters * sizeof(*rtts));
		char msg[MAXMSG] = { 0 };
		int wait = kind == K_RINGWAIT;
		pthread_t tid;

		if (!rtts)
			die("malloc rtts");
		atomic_store(&mb_ab.seq, 0);
		atomic_store(&mb_ba.seq, 0);
		if (kind == K_PIPE)
			setup_pipe(&e);
		else if (kind == K_UNIX)
			setup_unix(&e);
		else if (kind == K_TCP)
			setup_tcp(&e);

		pthread_barrier_init(&barrier, NULL, 2);
		if (pthread_create(&tid, NULL, lat_peer, &c))
			die("pthread_create");
		pin_to_cpu(cpu_a);
		pthread_barrier_wait(&barrier);
		for (uint64_t i = 0; i < iters + WARMUP; i++) {
			double t0 = now_sec();

			if (kind == K_RING || kind == K_RINGWAIT) {
				mbox_send(&mb_ab, msg, z, wait);
				mbox_recv(&mb_ba, msg, z, wait);
			} else {
				write_all(e.wfd[0], msg, z);
				read_msg(e.rfd[0], msg, z);
			}
			if (i >= WARMUP)
				rtts[i - WARMUP] = (now_sec() - t0) * 1e9;
		}
		pthread_join(tid, NULL);
		pthread_barrier_destroy(&barrier);
		if (e.rfd[0] != -1) {
			close_endpoint(&e, 0);
			close_endpoint(&e, 1);
		}

		csv_row("ipc", kname, label, cpu_a, cpu_b, -1, (long)z, rep,
			"rtt_med", median_of(rtts, (int)iters), "ns");
		csv_row("ipc", kname, label, cpu_a, cpu_b, -1, (long)z, rep,
			"rtt_p99", rtts[iters * 99 / 100], "ns");
		free(rtts);
	}
}

/* ---- throughput: one-way stream A -> B for a fixed window ---- */

struct spsc {
	_Atomic uint64_t head;
	char pad1[CACHELINE - 8];
	_Atomic uint64_t tail;
	char pad2[CACHELINE - 8];
	_Atomic int done;
	char pad3[CACHELINE - 4];
	char slots[];
};

static struct spsc *ring;

struct tput_ctx {
	int cpu;
	enum kind kind;
	struct endpoints *e;
	size_t z;
	uint64_t msgs;
	double elapsed;
};

static void *tput_consumer(void *arg)
{
	struct tput_ctx *c = arg;
	char msg[MAXMSG];
	uint64_t n = 0;
	double t0;

	pin_to_cpu(c->cpu);
	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	if (c->kind == K_RING) {
		uint64_t tail = 0;

		for (;;) {
			while (atomic_load_explicit(&ring->head,
						    memory_order_acquire)
			       == tail) {
				if (atomic_load_explicit(&ring->done,
							 memory_order_acquire)
				    && atomic_load_explicit(
					       &ring->head,
					       memory_order_acquire) == tail)
					goto out;
				cpu_relax();
			}
			memcpy(msg, ring->slots + (tail % RING_SLOTS) * c->z,
			       c->z);
			atomic_store_explicit(&ring->tail, ++tail,
					      memory_order_release);
			n++;
		}
	} else {
		while (read_msg(c->e->rfd[1], msg, c->z))
			n++;
	}
out:
	c->elapsed = now_sec() - t0;
	c->msgs = n;
	return NULL;
}

static void run_tput(enum kind kind, const char *kname, int cpu_a, int cpu_b,
		     size_t z, double window, int repeats, const char *label)
{
	for (int rep = 0; rep < repeats; rep++) {
		struct endpoints e = { .rfd = { -1, -1 }, .wfd = { -1, -1 } };
		struct tput_ctx c = { .cpu = cpu_b, .kind = kind, .e = &e,
				      .z = z };
		char msg[MAXMSG] = { 0 };
		pthread_t tid;
		double t0, rate;

		if (kind == K_RING) {
			ring = calloc(1, sizeof(*ring) + RING_SLOTS * z);
			if (!ring)
				die("calloc ring");
		} else if (kind == K_PIPE) {
			setup_pipe(&e);
		} else if (kind == K_UNIX) {
			setup_unix(&e);
		} else {
			setup_tcp(&e);
		}

		pthread_barrier_init(&barrier, NULL, 2);
		if (pthread_create(&tid, NULL, tput_consumer, &c))
			die("pthread_create");
		pin_to_cpu(cpu_a);
		pthread_barrier_wait(&barrier);
		t0 = now_sec();
		if (kind == K_RING) {
			uint64_t head = 0, tail_seen = 0;

			while (now_sec() - t0 < window) {
				for (int burst = 0; burst < 256; burst++) {
					while (head - tail_seen == RING_SLOTS)
						tail_seen = atomic_load_explicit(
							&ring->tail,
							memory_order_acquire);
					memcpy(ring->slots +
						       (head % RING_SLOTS) * z,
					       msg, z);
					atomic_store_explicit(
						&ring->head, ++head,
						memory_order_release);
				}
			}
			atomic_store_explicit(&ring->done, 1,
					      memory_order_release);
		} else {
			while (now_sec() - t0 < window)
				for (int burst = 0; burst < 64; burst++)
					write_all(e.wfd[0], msg, z);
			if (kind == K_PIPE)
				close(e.wfd[0]);
			else
				shutdown(e.wfd[0], SHUT_WR);
		}
		pthread_join(tid, NULL);
		pthread_barrier_destroy(&barrier);
		if (e.rfd[0] != -1) {
			close_endpoint(&e, 0);
			close_endpoint(&e, 1);
		}
		if (kind == K_RING) {
			free(ring);
			ring = NULL;
		}

		rate = (double)c.msgs / c.elapsed;
		csv_row("ipc", kname, label, cpu_a, cpu_b, -1, (long)z, rep,
			"tput_msgs", rate / 1e6, "Mmsg/s");
		csv_row("ipc", kname, label, cpu_a, cpu_b, -1, (long)z, rep,
			"tput_bw", rate * z / 1e6, "MB/s");
	}
}

int ipc_main(int argc, char **argv)
{
	int cpu_a = 0, cpu_b = 1, repeats = 5, tput = 0, opt;
	uint64_t iters = 100000;
	size_t z = 64;
	const char *kname = "ring", *label = "-";
	enum kind kind = K_RING;

	while ((opt = getopt(argc, argv, "a:b:k:z:n:r:l:T")) != -1) {
		switch (opt) {
		case 'a': cpu_a = atoi(optarg); break;
		case 'b': cpu_b = atoi(optarg); break;
		case 'k': kname = optarg; break;
		case 'z': z = (size_t)parse_size(optarg); break;
		case 'n': iters = strtoull(optarg, NULL, 10); break;
		case 'r': repeats = atoi(optarg); break;
		case 'l': label = optarg; break;
		case 'T': tput = 1; break;
		default: return 1;
		}
	}
	if (!strcmp(kname, "ring"))
		kind = K_RING;
	else if (!strcmp(kname, "ringwait"))
		kind = K_RINGWAIT;
	else if (!strcmp(kname, "pipe"))
		kind = K_PIPE;
	else if (!strcmp(kname, "unix"))
		kind = K_UNIX;
	else if (!strcmp(kname, "tcp"))
		kind = K_TCP;
	else
		die("bad kind '%s'", kname);
	if (z > MAXMSG)
		die("message size > %d", MAXMSG);
	if (kind == K_RING && cpu_a == cpu_b)
		die("spinning ring needs two distinct CPUs");
	if (tput && kind == K_RINGWAIT)
		die("ringwait is latency-only");

	if (tput)
		run_tput(kind, kname, cpu_a, cpu_b, z, 1.0, repeats, label);
	else
		run_latency(kind, kname, cpu_a, cpu_b, z, iters, repeats,
			    label);
	return 0;
}
