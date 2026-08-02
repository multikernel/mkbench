#!/usr/bin/env bash
# Detect topology, run the mkbench matrix across placement tiers, write CSVs.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MKB="$SCRIPT_DIR/../mkbench"
TUNE=0 QUICK=0 REPEATS=5 OUT=""

while [ $# -gt 0 ]; do
	case "$1" in
	--tune) TUNE=1 ;;
	--quick) QUICK=1 ;;
	--repeats) REPEATS=$2; shift ;;
	--out) OUT=$2; shift ;;
	*) echo "usage: $0 [--tune] [--quick] [--repeats N] [--out DIR]" >&2
	   exit 1 ;;
	esac
	shift
done

[ -x "$MKB" ] || { echo "error: $MKB not built; run make first" >&2; exit 1; }

expand_list() {
	local part a b out=""
	IFS=',' read -ra _parts <<< "$1"
	for part in "${_parts[@]}"; do
		if [[ $part == *-* ]]; then
			a=${part%-*}; b=${part#*-}
			out+=" $(seq -s' ' "$a" "$b")"
		else
			out+=" $part"
		fi
	done
	echo "$out"
}

join_comma() { local IFS=,; echo "$*"; }

# ---------------- topology ----------------
NODES=()
for d in $(ls -d /sys/devices/system/node/node[0-9]* | sort -V); do
	NODES+=("${d##*node}")
done
NODE0=${NODES[0]}
NODE1=${NODES[1]:-}

read -ra CPUS0 <<< "$(expand_list "$(cat /sys/devices/system/node/node"$NODE0"/cpulist)")"
A=${CPUS0[0]}

core_id() { cat "/sys/devices/system/cpu/cpu$1/topology/core_id"; }

T1P=""
read -ra SIBS <<< "$(expand_list "$(cat /sys/devices/system/cpu/cpu"$A"/topology/thread_siblings_list)")"
for c in "${SIBS[@]}"; do
	[ "$c" != "$A" ] && { T1P=$c; break; }
done

T2P=""
A_CORE=$(core_id "$A")
for c in "${CPUS0[@]}"; do
	[ "$(core_id "$c")" != "$A_CORE" ] && { T2P=$c; break; }
done

T3P=""
CPUS1=()
if [ -n "$NODE1" ]; then
	read -ra CPUS1 <<< "$(expand_list "$(cat /sys/devices/system/node/node"$NODE1"/cpulist)")"
	T3P=${CPUS1[0]}
fi

# one CPU per physical core, per socket
declare -A seen
SOCK0_CORES=()
for c in "${CPUS0[@]}"; do
	k=$(core_id "$c")
	[ -n "${seen[$k]:-}" ] && continue
	seen[$k]=1
	SOCK0_CORES+=("$c")
done
unset seen; declare -A seen
SOCK1_CORES=()
for c in "${CPUS1[@]:+"${CPUS1[@]}"}"; do
	k=s1$(core_id "$c")
	[ -n "${seen[$k]:-}" ] && continue
	seen[$k]=1
	SOCK1_CORES+=("$c")
done

SAME4="" SPLIT4=""
if [ ${#SOCK0_CORES[@]} -ge 4 ]; then
	SAME4=$(join_comma "${SOCK0_CORES[@]:0:4}")
fi
if [ ${#SOCK0_CORES[@]} -ge 2 ] && [ ${#SOCK1_CORES[@]} -ge 2 ]; then
	SPLIT4=$(join_comma "${SOCK0_CORES[@]:0:2}" "${SOCK1_CORES[@]:0:2}")
fi

echo "topology: node0=$NODE0 node1=${NODE1:-none} A=$A T1=${T1P:-skip} T2=${T2P:-skip} T3=${T3P:-skip}" >&2
[ -z "$NODE1" ] && echo "notice: single NUMA node; cross-socket (T3) and remote-memory runs skipped" >&2
[ -z "$T1P" ] && echo "notice: SMT off or single-thread cores; T1 skipped" >&2

# ---------------- results dir + environment ----------------
RES=${OUT:-$SCRIPT_DIR/../results/$(hostname)_$(date +%Y%m%d_%H%M%S)}
RAW="$RES/raw"
mkdir -p "$RES/env" "$RAW"

uname -a > "$RES/env/uname.txt"
lscpu > "$RES/env/lscpu.txt" 2>/dev/null
numactl --hardware > "$RES/env/numactl.txt" 2>/dev/null \
	|| grep -H . /sys/devices/system/node/node*/meminfo > "$RES/env/nodemem.txt" 2>/dev/null
cat /proc/cmdline > "$RES/env/cmdline.txt"
{
	echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
	echo "thp: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)"
	echo "numa_balancing: $(cat /proc/sys/kernel/numa_balancing 2>/dev/null || echo n/a)"
	echo "tune: $TUNE quick: $QUICK repeats: $REPEATS"
} > "$RES/env/settings.txt"

GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "")
NB=$(cat /proc/sys/kernel/numa_balancing 2>/dev/null || echo "")

restore() {
	if [ -n "$SAVED_GOV" ]; then
		for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
			echo "$SAVED_GOV" > "$g" 2>/dev/null
		done
	fi
	[ -n "$SAVED_NB" ] && echo "$SAVED_NB" > /proc/sys/kernel/numa_balancing 2>/dev/null
}
SAVED_GOV="" SAVED_NB=""

if [ "$TUNE" -eq 1 ]; then
	if [ "$(id -u)" -ne 0 ]; then
		echo "error: --tune needs root" >&2
		exit 1
	fi
	trap restore EXIT
	SAVED_GOV=$GOV SAVED_NB=$NB
	for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		echo performance > "$g" 2>/dev/null
	done
	[ -n "$NB" ] && echo 0 > /proc/sys/kernel/numa_balancing
	# Holding the fd keeps deep C-states disabled for the whole run.
	exec {DMAFD}<> /dev/cpu_dma_latency && printf '\0\0\0\0' >&$DMAFD
	echo "tuned: performance governor, numa_balancing=0, cpu_dma_latency=0" >&2
else
	[ "$GOV" != "performance" ] && [ -n "$GOV" ] && \
		echo "WARNING: cpufreq governor is '$GOV', not 'performance'; consider --tune" >&2
	[ "$NB" = "1" ] && \
		echo "WARNING: kernel.numa_balancing is enabled; consider --tune" >&2
fi

# ---------------- parameters ----------------
if [ "$QUICK" -eq 1 ]; then
	REPEATS=2
	LAT_SIZES="128k 16m 256m"
	BW1_SIZE=64m BWN_SIZE=32m
	BW_KINDS="read triad"
	PP_ITERS=300000
	RING_ITERS=50000 SLEEP_ITERS=10000 TCP_ITERS=5000
	WAKE_ITERS=10000 WIN_MS=200
	TLB_ITERS=3000 TLB_WIN=1000 TLB_COUNTS="1 2 4"
else
	LAT_SIZES="128k 16m 512m"
	BW1_SIZE=128m BWN_SIZE=64m
	BW_KINDS="read write copy triad"
	PP_ITERS=1000000
	RING_ITERS=100000 SLEEP_ITERS=20000 TCP_ITERS=10000
	WAKE_ITERS=20000 WIN_MS=500
	TLB_ITERS=20000 TLB_WIN=2000 TLB_COUNTS="1 2 4 8 16"
fi

run() {
	local csv="$RAW/$1.csv"
	shift
	echo "  mkbench $*" >&2
	"$MKB" "$@" >> "$csv" || echo "WARNING: mkbench $* failed" >&2
}

for t in memlat membw pingpong atomics lock ipc wakeup tlbshoot; do
	"$MKB" csvheader > "$RAW/$t.csv"
done

echo "== sanity check" >&2
"$MKB" check -c "$A" -m "$NODE0" >&2 || exit 1
[ -n "$T3P" ] && { "$MKB" check -c "$T3P" -m "$NODE1" >&2 || exit 1; }

echo "== memlat" >&2
for sz in $LAT_SIZES; do
	run memlat memlat -c "$A" -m "$NODE0" -s "$sz" -r "$REPEATS" -l local
	[ -n "$NODE1" ] && \
		run memlat memlat -c "$A" -m "$NODE1" -s "$sz" -r "$REPEATS" -l remote
done

echo "== membw" >&2
SOCK0_LIST=$(join_comma "${SOCK0_CORES[@]}")
for k in $BW_KINDS; do
	for cfg in "$A:$BW1_SIZE" "$SOCK0_LIST:$BWN_SIZE"; do
		cpus=${cfg%%:*} sz=${cfg##*:}
		run membw membw -c "$cpus" -m "$NODE0" -s "$sz" -k "$k" -r "$REPEATS" -l local
		if [ -n "$NODE1" ]; then
			run membw membw -c "$cpus" -m "$NODE1" -s "$sz" -k "$k" -r "$REPEATS" -l remote
			run membw membw -c "$cpus" -m -2 -s "$sz" -k "$k" -r "$REPEATS" -l interleave
		fi
	done
done

echo "== pingpong" >&2
for tier in "T1:$T1P" "T2:$T2P" "T3:$T3P"; do
	t=${tier%%:*} p=${tier##*:}
	[ -z "$p" ] && continue
	run pingpong pingpong -a "$A" -b "$p" -k rtt -n "$PP_ITERS" -r "$REPEATS" -l "$t"
	run pingpong pingpong -a "$A" -b "$p" -k false-shared -r "$REPEATS" -l "$t"
	run pingpong pingpong -a "$A" -b "$p" -k false-padded -r "$REPEATS" -l "$t"
done

echo "== atomics" >&2
run atomics atomics -c "$A" -n "$WIN_MS" -r "$REPEATS" -l 1t
[ -n "$SAME4" ] && run atomics atomics -c "$SAME4" -n "$WIN_MS" -r "$REPEATS" -l sock0
[ -n "$SPLIT4" ] && run atomics atomics -c "$SPLIT4" -n "$WIN_MS" -r "$REPEATS" -l split

echo "== lock" >&2
for k in mutex spin; do
	[ -n "$T2P" ] && run lock lock -c "$A,$T2P" -k "$k" -n "$WIN_MS" -r "$REPEATS" -l T2
	[ -n "$T3P" ] && run lock lock -c "$A,$T3P" -k "$k" -n "$WIN_MS" -r "$REPEATS" -l T3
	[ -n "$SAME4" ] && run lock lock -c "$SAME4" -k "$k" -n "$WIN_MS" -r "$REPEATS" -l sock0
	[ -n "$SPLIT4" ] && run lock lock -c "$SPLIT4" -k "$k" -n "$WIN_MS" -r "$REPEATS" -l split
done

echo "== ipc latency" >&2
for tier in "T0:$A" "T1:$T1P" "T2:$T2P" "T3:$T3P"; do
	t=${tier%%:*} p=${tier##*:}
	[ -z "$p" ] && continue
	[ "$t" != "T0" ] && \
		run ipc ipc -a "$A" -b "$p" -k ring -n "$RING_ITERS" -r "$REPEATS" -l "$t"
	run ipc ipc -a "$A" -b "$p" -k ringwait -n "$SLEEP_ITERS" -r "$REPEATS" -l "$t"
	run ipc ipc -a "$A" -b "$p" -k pipe -n "$SLEEP_ITERS" -r "$REPEATS" -l "$t"
	run ipc ipc -a "$A" -b "$p" -k unix -n "$SLEEP_ITERS" -r "$REPEATS" -l "$t"
	run ipc ipc -a "$A" -b "$p" -k tcp -n "$TCP_ITERS" -r "$REPEATS" -l "$t"
done

echo "== ipc throughput" >&2
for tier in "T2:$T2P" "T3:$T3P"; do
	t=${tier%%:*} p=${tier##*:}
	[ -z "$p" ] && continue
	for k in ring pipe unix tcp; do
		run ipc ipc -a "$A" -b "$p" -k "$k" -T -z 4096 -r "$REPEATS" -l "$t"
	done
done

echo "== wakeup" >&2
for tier in "T0:$A" "T1:$T1P" "T2:$T2P" "T3:$T3P"; do
	t=${tier%%:*} p=${tier##*:}
	[ -z "$p" ] && continue
	run wakeup wakeup -a "$A" -b "$p" -n "$WAKE_ITERS" -r "$REPEATS" -l "$t"
done

# Participants exclude the initiator A. Both lists hold the participant count
# fixed so the only variable between sock0 and split is placement.
tlb_same_list() {
	local n=$1
	[ $((${#SOCK0_CORES[@]} - 1)) -ge "$n" ] || return 1
	join_comma "${SOCK0_CORES[@]:1:$n}"
}

tlb_split_list() {
	local n=$1 h0 h1
	h0=$((n / 2)); h1=$((n - h0))
	[ ${#SOCK1_CORES[@]} -ge "$h1" ] || return 1
	[ $((${#SOCK0_CORES[@]} - 1)) -ge "$h0" ] || return 1
	if [ "$h0" -eq 0 ]; then
		join_comma "${SOCK1_CORES[@]:0:$h1}"
	else
		join_comma "${SOCK0_CORES[@]:1:$h0}" "${SOCK1_CORES[@]:0:$h1}"
	fi
}

echo "== tlbshoot" >&2
run tlbshoot tlbshoot -a "$A" -n "$TLB_ITERS" -r "$REPEATS" -l solo
for n in $TLB_COUNTS; do
	if same=$(tlb_same_list "$n"); then
		run tlbshoot tlbshoot -a "$A" -c "$same" -n "$TLB_ITERS" -r "$REPEATS" -l sock0
	fi
	if split=$(tlb_split_list "$n"); then
		run tlbshoot tlbshoot -a "$A" -c "$split" -n "$TLB_ITERS" -r "$REPEATS" -l split
	fi
done

# 64 pages crosses tlb_single_page_flush_ceiling (33 on x86), where the kernel
# switches from per-page invalidation to a full flush.
for cfg in "mprotect:64" "dontneed:1"; do
	o=${cfg%%:*} p=${cfg##*:}
	if same=$(tlb_same_list 4); then
		run tlbshoot tlbshoot -a "$A" -c "$same" -o "$o" -p "$p" -n "$TLB_ITERS" -r "$REPEATS" -l sock0
	fi
	if split=$(tlb_split_list 4); then
		run tlbshoot tlbshoot -a "$A" -c "$split" -o "$o" -p "$p" -n "$TLB_ITERS" -r "$REPEATS" -l split
	fi
done

# Victim-side disturbance, each placement paired with a zero-shootdown control
# so the noise floor is visible next to the effect.
for k in jitter control; do
	if same=$(tlb_same_list 4); then
		run tlbshoot tlbshoot -a "$A" -c "$same" -k "$k" -w "$TLB_WIN" -r "$REPEATS" -l sock0
	fi
	if split=$(tlb_split_list 4); then
		run tlbshoot tlbshoot -a "$A" -c "$split" -k "$k" -w "$TLB_WIN" -r "$REPEATS" -l split
	fi
done

echo "== done: $RES" >&2
if command -v python3 > /dev/null; then
	python3 "$SCRIPT_DIR/summarize.py" "$RES" | tee "$RES/summary.txt"
else
	echo "python3 not found; run scripts/summarize.py $RES elsewhere" >&2
fi
