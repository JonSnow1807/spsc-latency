# Validation on the x86 Linux box

AMD Ryzen Threadripper PRO 3995WX (64 cores / 128 threads, 1 socket), Linux
6.8.0-138-generic, Ubuntu 24.04.5 container on a **shared rented Vast.ai host**.
Normal builds use GCC 13.3.0 (`c++`); the ThreadSanitizer run uses Clang 21.1.8
(see below). The master script is `job.sh`; every command ran under
`taskset -c 2-7`. Timings in correctness logs are not benchmark results.

| Check | Command / budget | Log | Result |
|---|---|---|---|
| Release stress | `./build-linux/spsc_tests --items 100000000` (run by `scripts/run_linux.sh`, each of four designs) | `tests.txt` | PASS |
| ThreadSanitizer | `TSAN_OPTIONS=halt_on_error=1 ./build-tsan-clang/spsc_tests --items 100000000 --timeout-seconds 1200` (each design), Clang 21.1.8 build | `build-tsan-clang.txt`, `tests-tsan.txt` | PASS, first attempt of the retry loop |
| CTest | `ctest --test-dir build --output-on-failure` | `build-normal.txt`, `tests-ctest.txt` | 2/2 passed |
| Capacity one | `./build/spsc_tests --items 1000000 --capacity 1` | `tests-capacity-one.txt` | PASS |
| Benchmark | `JOBS=4 taskset -c 2-7 ./scripts/run_linux.sh 2 3` | `run_linux-console.txt`, `run.txt`, `summary.md`, `measurements.csv` | 120 trials, "Run complete" footer |

ASan/UBSan, plain-Make and harness-smoke checks from the Mac run were not
repeated here.

## ThreadSanitizer on this kernel

`vm.mmap_rnd_bits` is 32 and the container cannot disable ASLR
(`setarch -R` fails with "Operation not permitted"). The GCC 13 TSan build
(`build-tsan-gcc.txt`) aborts at startup with "unexpected memory mapping"
(`tests-tsan-gcc-setarch-failed.txt`, `tests-tsan-gcc-failed.txt`). The Clang 21
runtime can work with this layout, but on some launches it still aborts before
any test with "incompatible memory layout" (exit 66,
`tests-tsan-clang-first-attempt.txt`). `tsan.sh` therefore retries only that
startup abort, logging every attempt; the logged attempt ran to completion on its
first try and all four designs passed 100 million items. `compile_commands-tsan.json`
is the Clang build.

## Placement and host load

`run_linux.sh 2 3` puts the producer/initiator on CPU 2 and the
consumer/reflector on CPU 3; the bench verifies each singleton affinity mask.
CPUs 2 and 3 are separate physical cores in the same L3 complex (L3 shared by
0-3 and 64-67); their SMT siblings are 66 and 67. `corebusy.log` samples busy
time of CPUs 2-7 and 66-71 for 5 s every 30 s during the whole job. CPUs 66 and 67
showed 0.0% in every sample; CPU 7 was 100% busy in every sample, including the
one taken before any build or test started (another tenant; the bench used
only CPUs 2 and 3). The host-wide 1-minute load average
went from 23.74 to 17.67 over the job (`corebusy.log`). The governor was
`schedutil`; frequency, boost, IRQ placement and isolation were not controlled.

The repository path on the box was replaced by `<repo>` in saved logs and
compiler commands. `run_linux.sh` wrote `results/linux-ff8f5867e9a6`, which was
renamed to this directory. `source-sha256.txt` matches the committed sources.
`summary.md` is the bench's own output with a medians section appended
afterwards by `spsc_medians.py`.
