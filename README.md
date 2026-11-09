# SPSC ring buffers and latency measurements

A small C++20 study of bounded single-producer/single-consumer queues. Everything
here is implemented locally: no queue libraries, histogram libraries, fetched
dependencies, or generated benchmark data. The comparison is between our own
implementations of established designs, not binaries from the upstream projects.

## Queues

| Header / type in `spsc` | Index placement | Ordering | Peer-index cache |
|---|---|---|---|
| `naive.hpp` / `naive<T>` | Same cache line | All operations `seq_cst` | No |
| `acqrel.hpp` / `acqrel<T>` | Same cache line | Acquire/release | No |
| `padded.hpp` / `padded<T>` | Separate cache lines | Acquire/release | No |
| `cached.hpp` / `cached<T>` | Separate cache lines | Acquire/release | Yes |

`acqrel` is an experimental control: comparing it with `padded` changes only
index separation. Comparing `naive` with `padded` changes ordering **and** layout.
`padded` and `cached` share the same index offsets and object size. The three
requested designs plus this control share one compile-time implementation to
keep allocation, element handling, and indexing identical. Batching is omitted.

```cpp
#include <memory>
#include "spsc/cached.hpp"

spsc::cached<std::unique_ptr<int>> q(1024); // exactly 1024 usable slots
auto value = std::make_unique<int>(42);
bool pushed = q.try_push(std::move(value)); // producer only
std::unique_ptr<int> received;
bool popped = q.try_pop(received);          // consumer only
```

Construction takes a nonzero power-of-two capacity, at most `SIZE_MAX/2`, with a
separate allocation-size overflow check. Counters are unsigned, monotonically
advancing modulo their range; the slot is `counter & (capacity - 1)`. This uses
all slots, including capacity one. Producer progress beyond its last observed
consumer counter is bounded by capacity. Consumer progress is bounded by the
last observed producer counter. Thus the modular full/empty checks remain valid
across counter overflow; a stale cache cannot authorize a second lap.

Storage is allocated once, supports over-aligned elements, and is explicitly
constructed and destroyed. Elements need not be default-constructible or
copyable, but must have nonthrowing move construction, move assignment and
destruction. `try_emplace` may throw from an element constructor without
publishing a slot. A full push does not move from its argument; an empty pop
leaves its output unchanged. The queue is neither copyable nor movable.
Destruction destroys outstanding elements and requires both threads to have
stopped. Exactly one thread may call the producer API and one the consumer API;
role handoff requires external synchronization. A failed try is a transient
observation, not a snapshot of the queue's current global state.

The ownership and synchronization argument is deliberately short:

* Producer constructs a slot, then release-stores the write counter. Consumer
  acquire-loads that counter before moving from the slot.
* Consumer moves from and destroys the slot, then release-stores the read
  counter. Producer acquire-loads that counter before reusing the storage.
* An owner alone writes its index, so its own loads can be relaxed. Sequential
  consistency in `naive` is stronger than necessary. Cached observations retain
  the preceding acquire's permission to access the slots it covered. The peer
  counter is reloaded only on apparent full/empty.

Every acquire/release use has a reason at its source location. Cached copies are
ordinary owner-only variables in the owner's index block. Compile-time assertions
check atomic lock freedom, index alignment, and the actual member offsets.
Apple arm64 uses 128-byte separation; other targets default to 64. Override with
`-DSPSC_CACHE_LINE=128` in compiler flags if necessary, consistently across all
translation units. This explicit choice avoids toolchain-dependent interference
size constants and their ABI differences. Element slots are contiguous, not
individually cache-line padded: producer/consumer access to nearby payloads can
still cause false sharing. Queue coordination allocates and locks nothing after
construction; user-supplied element operations may do either.

### Design credits

* The basic single-writer circular-buffer protocol is the conventional Lamport
  SPSC design. Our separated acquire/release variant follows the same
  publication/reclamation protocol used in
  [Boost.Lockfree's `spsc_queue`](https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/spsc_queue.hpp)
  and [Folly's `ProducerConsumerQueue`](https://github.com/facebook/folly/blob/main/folly/ProducerConsumerQueue.h).
* The owner-local cached peer indices and reload-on-full/empty optimization are
  credited to [Erik Rigtorp's `SPSCQueue`](https://github.com/rigtorp/SPSCQueue).
* The fixed relative precision histogram is inspired by
  [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram); it is our own
  small implementation and is not format/API compatible.

These are independently written implementations of the stated algorithms.
Compared with the referenced implementations, we deliberately use power-of-two
full-capacity unsigned counters, a shared storage implementation, and a small
move-based API. We do not reproduce their entire APIs, padding policies,
allocators, or batching options, or claim to benchmark their released code.
The source links are credits; building and testing require no network.

## Build and correctness

Apple clang 17 was used here. GCC/Clang on Linux with C++20 library support is
also targeted. CMake probes `-march=native`; the fallback Makefile probes it too.
Both use C++20, `-O3`, `-Wall -Wextra -Wpedantic -Werror` for normal builds.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/spsc_tests --items 100000000

cmake -S . -B build-tsan -DSPSC_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j 4
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/spsc_tests --items 100000000 --timeout-seconds 1200

# Plain Make, without CMake; CXX=g++ is also supported.
make -j 4
make test
make stress
make BUILD=out-tsan TSAN=1 -j 4
TSAN_OPTIONS=halt_on_error=1 ./out-tsan/spsc_tests --items 1000000
```

Use separate build directories for sanitizer/configuration changes. `NATIVE=0`
(Make) or `-DSPSC_NATIVE=OFF` (CMake) disables native tuning. Sanitized builds use
`-O1 -g -fsanitize=thread -fno-omit-frame-pointer`; do not compare their timings.
The Makefile does not track changes to command-line flags: use a new `BUILD`
directory or clean when changing them.

The default stress test sends 100 million 64-byte messages **per design** and
checks every sequence and payload word. `--items N` and `--capacity N` configure
it. A watchdog fails a stalled test after 600 seconds (configurable). Unit tests
exercise capacity one, full/empty transitions, FIFO order, failed operations,
move-only ownership, destruction, constructor exceptions, overflow rejection,
over-alignment, and histogram accuracy against a sorted reference. An internal
8-bit counter policy drives concurrent actual counter overflow hundreds of
times with the production algorithm, including cache refreshes; public types
always use `size_t`. Checks remain enabled under `NDEBUG`. `ctest` uses one
million stress items for a quick check. TSan exercises the weaker arm64 target;
neither stress nor TSan constitutes a proof of the memory-ordering argument.

## Benchmark method

```sh
./build/spsc_bench --items 10000000 --samples 1000000 --warmup 100000 \
  --capacities 64,1024,65536 --repeats 3 --seed 20261108 --output results/my-run
```

Each case runs both tests with exactly one producer and one consumer, for 8-byte
and 64-byte messages. Throughput starts in a barrier completion after warmup and
ends after the last consumer pop/validation, including drain time. It counts
delivered messages per second, not enqueue-plus-dequeue operations. Generation,
full-payload validation, and retries are timed; construction and joining of
threads are not. This is an end-to-end message loop, not an isolated instruction
benchmark. All payload words vary with sequence and are checked.

Latency uses two queues and one outstanding message. The initiator takes two
`steady_clock` timestamps around sending a request and receiving the reflected
response. **Half round-trip latency (RTT/2)** includes queue transfers, responder
turnaround, polling, scheduling, and clock overhead. It cannot independently
measure one-way latency, distinguish the two directions, or measure just
`try_push`. Full RTT nanoseconds are histogrammed before division, preserving
half-nanosecond reporting. Reply validation and histogram updates occur outside
the measured interval but affect cache state and the next request's arrival.

The histogram preallocates 55 × 1024 64-bit counters (~440 KiB). It represents
0–2047 ns exactly and has 1024 buckets per subsequent power-of-two interval.
Quantiles use exact integer/rational nearest ranks (avoiding floating-point
rounding of p99.9) and return bucket upper bounds, clamped to the exact
observed maximum: relative value approximation is below 1/1024 (~0.098%) above
2047 ns. That is **not** a statistical confidence bound. The full-RTT maximum is
retained exactly at nanosecond clock conversion precision. Back-to-back clock
readings are reported separately, never subtracted from measured percentiles.
Clock period and observed overhead are not promises of clock resolution.

Warmup is per test; choose at least the largest ring capacity. Slots and histogram
storage are initialized before timing. All designs use the same spin hint
(`yield` on arm64, `pause` on x86). Case order, including repetitions, is shuffled
using a fixed `mt19937_64` seed and specified rejection/Fisher–Yates mapping, so
the order is stable across standard libraries. Payloads themselves are deterministic.
The benchmark reports p50/p90/p99/p99.9/p99.99/max in ns, per-trial throughput,
compiler, flags, placement, counts, seed, and cache-line assumption in CSV and
Markdown. It refuses to overwrite existing measurements. A partial run has no
"Run complete" footer. Raw sample streams are not retained.

This is closed-loop traffic, not fixed offered load. A scheduling stall suppresses
new arrivals, so the tail cannot predict an open-loop exchange feed under load;
no coordinated-omission correction is applied. In ping-pong, capacity changes
slot reuse distance and memory footprint, not backlog. Throughput can fill the
ring and benefits much more from cached peer indices. Tail percentiles from
short trials are unstable: 200,000 samples leave only about 20 observations
beyond p99.99. Repeated trial percentiles are kept separate, never treated as
pooled samples. CPU migration, heterogeneous cores, frequency, thermals, and
other applications remain sources of noise.

## Measured Mac run

Results and validation logs are in [`results/macbook-m2`](results/macbook-m2/).
This is a **laptop run with no pinning**, on an Apple M2 Pro, 10 cores (6 performance,
4 efficiency), 16 GB RAM, macOS 15.5, Apple clang 17. macOS has no supported strict
CPU-affinity API equivalent to Linux's; affinity tags are not pinning. Both worker
threads request `QOS_CLASS_USER_INTERACTIVE`, relative priority 0, and failures
are fatal. QoS influences scheduling priority but does not select cores, prevent
migration, or fix frequency. `--qos default` leaves the existing policy alone.
`--cpu-a/--cpu-b` are rejected on macOS.

The short sweep used 2,000,000 throughput messages and 200,000 measured RTTs
per trial, 100,000 warmups, capacities 64/1024/65536, both payload sizes, and
three shuffled repetitions: 72 trials total. All payload checks passed.
Normal and TSan stress tests each passed 100 million transfers per design.

**8-byte messages, capacity 1024.** Entries are medians of the three separate
trial statistics, not percentiles of pooled samples. Latency is RTT/2 in ns.

| Design | M items/s | p50 ns | p99 ns | p99.9 ns | Trial p99.9 range (ns) |
|---|---:|---:|---:|---:|---:|
| naive | 17.565 | 83.5 | 104.5 | 125 | 125–146 |
| acqrel | 44.762 | 83.5 | 104 | 125 | 125–1854.5 |
| padded | 45.230 | 83 | 375 | 417 | 396–417 |
| cached | 147.816 | 42 | 62.5 | 104.5 | 83.5–125 |

The adjacent acquire/release control reached 44.762 M items/s versus padded's
45.230 M items/s for this 8-byte case, only about 1% apart. For **64-byte
messages at the same capacity**, the medians were 20.227 M items/s (`acqrel`),
27.451 (`padded`), and 159.750 (`cached`). The layout-only comparison was about
36% faster with padding there, consistent with less index false sharing.
That mechanism does not imply a universal speedup: payload cache-line sharing
and scheduling remain, and the 8-byte case showed little separation benefit.

Cached peer indices improved bulk throughput by 3.27× over padded for 8-byte
messages and 5.82× for 64-byte messages at capacity 1024. The producer can use
already-observed free space, and the consumer can drain already-published
elements, without reloading the peer index on every operation. This benefit
depends on occupancy and scheduling. For example, cached 8-byte throughput
fell from 370.570 M items/s at capacity 64 to 34.980 at capacity 65536 in this
run. A larger ring did not reliably help. The experiment does not isolate
cache footprint from core placement or frequency effects.

Tail latency does not follow the throughput ranking: padded's 8-byte p99 was
375 ns versus naive's 104.5 ns; one `acqrel` trial reached p99.9 of 1854.5 ns.
For 64-byte messages, cached and padded both had a 62.5 ns median of trial
p50s, while their p99.9 medians were 146 ns and 125 ns respectively. These
are observations from a short unpinned laptop run, not evidence of a stable
tail-latency ordering.

Back-to-back clock readings had p50 **0 ns**, p99 **42 ns**, and max 125 ns.
A zero median means many readings landed on the same clock value; it does
not mean timing is free. The nominal nanosecond period and half-nanosecond
formatted values do not imply that resolution. Small differences such as
83 versus 83.5 ns should not be interpreted as meaningful improvements.
Throughput windows were only about 5.0–137.1 ms. This is a short reproducibility
and comparison run; longer repeated measurements on controlled Linux cores
are needed before using the numbers for engineering decisions.

Reproduce the Mac workload in a fresh directory:

```sh
./build/spsc_bench --items 2000000 --samples 200000 --warmup 100000 \
  --capacities 64,1024,65536 --repeats 3 --seed 20261108 --output results/mac-repeat
```

The saved command, full per-trial table and clock-overhead measurements are in
[`summary.md`](results/macbook-m2/summary.md); machine details, compiler commands,
normal/TSan test logs, and the CSV are alongside it. The repository was initially
empty; all work remains uncommitted for review.

## Measured x86 Linux run (pinned)

Results and logs are in [`results/x86-threadripper-3995wx`](results/x86-threadripper-3995wx/).
The machine was an **AMD Ryzen Threadripper PRO 3995WX** (64 cores / 128 threads)
on a **shared rented Vast.ai host**, with Linux 6.8 and GCC 13.3.0
(`-O3 -march=native`, 64-byte separation). The command was
`taskset -c 2-7 ./scripts/run_linux.sh 2 3`, so the producer/initiator was pinned to CPU 2
and the consumer/reflector to CPU 3. These are separate physical cores sharing one L3
complex, and the bench verified both affinity masks. The run used the script's default
sweep: capacities 2/8/64/1024/65536, both payload sizes, 10,000,000 throughput
messages and 1,000,000 RTT samples per trial, 100,000 warmups, three shuffled
repetitions, for 120 trials.

Other tenants kept the host busy. The 1-minute load average went from 23.74 to 17.67
during the job, and one nearby CPU (7) was fully busy throughout. Sampled busy time on
the SMT siblings of CPUs 2 and 3 stayed at 0.0%. Frequency, boost, interrupts and
isolation were not controlled. Release stress (`spsc_tests`) and ThreadSanitizer (Clang 21)
each passed 100 million transfers per design. GCC 13's TSan runtime aborted at startup on this
host, where `vm.mmap_rnd_bits` is 32 and the container cannot disable ASLR. See [`validation.md`](results/x86-threadripper-3995wx/validation.md).

**8-byte messages, capacity 1024.** Medians of three trial statistics, with RTT/2 in ns.

| Design | M items/s | p50 ns | p99 ns | p99.9 ns | Trial M items/s | Trial p99.9 (ns) |
|---|---:|---:|---:|---:|---|---|
| naive | 11.150 | 145.5 | 160.5 | 170 | 11.353, 11.150, 11.135 | 170, 170, 165.5 |
| acqrel | 40.448 | 145 | 160 | 170.5 | 40.079, 40.532, 40.448 | 170.5, 170.5, 170.5 |
| padded | 97.428 | 110 | 125 | 140 | 97.782, 97.428, 97.326 | 140, 150.5, 140 |
| cached | 133.752 | 110 | 135.5 | 140.5 | 134.992, 133.752, 100.716 | 140.5, 140.5, 140.5 |

For **64-byte messages at capacity 1024**, the throughput medians were 10.067 M items/s
(`naive`), 54.187 (`acqrel`), 36.500 (`padded`) and 42.857 (`cached`). Unlike on the Mac,
the adjacent-index `acqrel` control was the fastest design in this case. The x86
throughput ranking also depends on capacity. For example, cached 8-byte throughput was
133.752 M items/s at capacity 1024, 88.974 at 64, and 9.548 at 2. Back-to-back
`steady_clock` pairs had p50 40 ns, p99 41 ns and max 50,124 ns, and that clock
overhead is inside every RTT. Per-trial values for every case are in the medians section of
[`summary.md`](results/x86-threadripper-3995wx/summary.md). That file also has the raw table,
and `measurements.csv` and `machine.txt` sit alongside it.

## Pinned Linux run

Inspect topology and the allowed CPU list before choosing two CPUs:

```sh
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
sed -n '/Cpus_allowed_list/p' /proc/self/status
./scripts/run_linux.sh 2 4   # example only: choose actual allowed CPUs
```

The script builds, runs the 100-million-item correctness test, then a full sweep
of capacities 2, 8, 64, 1024, 65536 and both element sizes, three repetitions,
one million latency samples and ten million throughput messages per case. It
writes `results/linux-<hostname>/`, recording topology, compiler commands and
logs, and refuses to replace an existing directory. Environment variables
`STRESS_ITEMS`, `ITEMS`, `SAMPLES`, `WARMUP`, `REPEATS`, `CAPACITIES`, and `JOBS`
override budgets. For a direct run use `--cpu-a A --cpu-b B`; both must be supplied
and distinct. The initiator/producer uses A; the reflector/consumer uses B.
`pthread_setaffinity_np` is checked, and `pthread_getaffinity_np` must confirm
the exact singleton mask. Failed placement is fatal, not silently unpinned.

For within-socket comparisons choose separate physical cores in the same NUMA
node; SMT siblings and cross-socket placement answer different questions. Record
those choices. First touch happens on the initiator; this is not an explicit
NUMA allocation policy. Affinity alone does not isolate CPUs from interrupts or
other tasks. Use a quiet machine and record governor, power/boost policy, and
isolation settings alongside the output when controlling them. Run TSan
separately from performance measurements. Linux runtime results are not claimed
by this Mac run.
