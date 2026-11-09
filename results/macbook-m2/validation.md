# Validation on this Mac

Apple M2 Pro / macOS 15.5 / Apple clang 17. All commands exited 0 unless a
deliberately invalid invocation was being tested. Timings in correctness logs
are not benchmark results; some correctness/build jobs ran concurrently.

| Check | Command / budget | Log |
|---|---|---|
| Release stress | `./build/spsc_tests --items 100000000` (each of four designs, 64-byte payload) | `tests-normal.txt` |
| ThreadSanitizer | `TSAN_OPTIONS=halt_on_error=1 ./build-tsan/spsc_tests --items 100000000 --timeout-seconds 1200` (each design) | `tests-tsan.txt` |
| TSan after histogram fix | `TSAN_OPTIONS=halt_on_error=1 ./build-tsan/spsc_tests --items 1000000` (each design plus percentile regression tests) | `tests-tsan-final.txt` |
| CTest | `ctest --test-dir build --output-on-failure` | `tests-ctest.txt` |
| Capacity one | `./build/spsc_tests --items 1000000 --capacity 1` | `tests-capacity-one.txt` |
| Plain Make | `make -j 4` then `./out/spsc_tests --items 1000000` | `build-make.txt`, `tests-make.txt` |
| ASan + UBSan | `make BUILD=out-asan NATIVE=0 CXXFLAGS='-g -fsanitize=address,undefined -fno-omit-frame-pointer' -j 4` then `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./out-asan/spsc_tests --items 1000000` | `build-asan-ubsan.txt`, `tests-asan-ubsan.txt` |
| Harness smoke | 1000 items/samples, 64 warmup, capacities 1/2, one trial, four designs, both sizes; parsed CSV and checked failures for invalid CLI inputs and overwrites | `tests-harness.txt` |
| Linux runner | `bash -n scripts/run_linux.sh` | Syntax only; Linux runtime not available here |

Every C++ test invocation also runs lifetime, alignment, full/empty, exception,
histogram-reference, and concurrent narrow-counter wrap tests. Release checks
do not use `assert`, so `NDEBUG` does not disable validation.

This Apple AddressSanitizer runtime rejects `detect_leaks=1`; leak detection was
disabled for the successful supplementary ASan/UBSan run. TSan needed no runtime
workaround. Compiler commands are retained with the repository's absolute path
replaced by `<repo>`; `source-sha256.txt` identifies the reviewed source snapshot.

The Codex design critique led to the `acqrel` layout control. The deep code review
found floating-point nearest-rank overshoot for p99.9 on Apple arm64. This was
fixed with exact rational arithmetic and outlier boundary tests at 1,000,
10,000 and 1,000,000 samples before collecting performance results. Queue code
was unchanged after the full 100-million-item TSan pass. The release stress test
was rerun at 100 million after the histogram fix, and TSan at one million.
The focused follow-up review found no issue with the exact rank formula or its
overflow bounds. The final 72-row measurement CSV was checked for a complete
case/trial matrix, expected counts, monotonic quantiles and completion footer.
