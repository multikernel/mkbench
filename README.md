# Multikernel Microbenchmarks

Microbenchmarks that quantify what it costs to cross the socket/NUMA boundary
on a multi-socket machine: memory latency and bandwidth, cache-coherence
traffic, IPC transports, and sleep/wake paths. Built to inform multikernel
designs where each kernel owns a socket and kernels talk over shared memory
plus an IPI doorbell.

## Requirements

- Build: gcc (or clang) and make. No libraries beyond pthreads; NUMA syscalls
  (`mbind`, `move_pages`) are invoked directly, so libnuma is not needed.
- Run: Linux with sysfs. `python3` is optional (summary tables). Root is
  optional (`--tune`).

## Quick start

```sh
make
rsync -a . testbox:mkbench/
ssh testbox 'cd mkbench && sudo scripts/run_all.sh --tune'
rsync -a testbox:mkbench/results/ results/
```

`run_all.sh` autodetects topology, picks representative CPU pairs, runs the
whole matrix, and writes `results/<host>_<timestamp>/` containing `env/`
(machine state), `raw/*.csv`, and `summary.txt`. Use `--quick` for a fast
smoke run, `--repeats N` to change repetition count.

On a single-node machine the cross-socket and remote-memory runs are skipped
with a notice, so the suite can be exercised anywhere.

## Placement tiers

| Tier | Meaning |
|------|---------|
| T0 | both tasks on one CPU (context-switch baseline, blocking IPC only) |
| T1 | SMT siblings of one core |
| T2 | different cores, same socket (shared LLC, same NUMA node) |
| T3 | different sockets (interconnect + remote cache/memory) |

Memory tests use `local` / `remote` / `interleave` labels instead: the thread
stays on node 0 while the buffer is bound to node 0, node 1, or interleaved.

## Tests

| Test | What it measures |
|------|------------------|
| memlat | dependent pointer-chase load latency at L2/LLC/DRAM sizes |
| membw | STREAM-style read/write/copy/triad bandwidth, 1 thread and one per core |
| pingpong | cache-line round trip between two spinning cores; false-sharing variants |
| atomics | contended `fetch_add` on one cache line |
| lock | mutex/spinlock handoff and contention |
| ipc | round-trip latency and streaming throughput over shared-memory ring (spin and futex-doorbell modes), pipe, UNIX socket, TCP loopback |
| wakeup | futex wake of a sleeping partner (includes the cross-CPU IPI) |

The futex-doorbell ring (`ipc -k ringwait`) is the closest userspace model of
a cross-kernel channel: shared-memory mailbox plus a wake IPI. Compare its T2
vs T3 rows to estimate what a multikernel pays for cross-socket messaging,
and against `ring` (spin mode) for the doorbell overhead itself.

Each test is also directly invokable, e.g.:

```sh
./mkbench memlat -c 0 -m 1 -s 512m -r 5 -l remote
./mkbench ipc -a 0 -b 32 -k ringwait -z 64 -n 20000 -l T3
./mkbench check -c 32 -m 1     # verify pinning + binding work
```

All output is CSV on stdout:
`test,variant,tier,cpu_a,cpu_b,mem_node,size,repeat,metric,value,unit`.

## Expected ballparks (typical 2-socket x86 server)

| Metric | Same socket | Cross socket |
|--------|-------------|--------------|
| DRAM load latency | 80-110 ns | 130-220 ns (1.5-2x) |
| Cache-line RTT (pingpong) | 40-80 ns | 100-300 ns |
| Contended atomic (ns/op, 4t) | 50-150 ns | 2-5x same-socket |
| Ring RTT (spin, 64 B) | 100-250 ns | 250-700 ns |
| Futex wakeup | 1-4 us | 2-8 us |
| Triad bandwidth (per socket) | full local BW | 30-70% of local |

If numbers are wildly off, check `env/settings.txt` in the results dir:
- governor not `performance` or deep C-states enabled: inflated sleep/wake
  and latency numbers (fix with `--tune`)
- `numa_balancing` enabled: pages migrate mid-run and blur local vs remote
- THP disabled: DRAM latency rises from TLB misses (still valid, just note it)
- another workload running: bandwidth and contention numbers are polluted

## Out of scope (v1)

Raw IPI latency (needs a kernel module), NIC IRQ placement experiments
(needs a traffic peer), and plot generation.
