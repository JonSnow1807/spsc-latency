#!/bin/bash
# TSan 100M run with clang 21. The container cannot disable ASLR (personality is
# blocked) and vm.mmap_rnd_bits=32, so the TSan runtime sometimes aborts at
# startup with "incompatible memory layout" (exit 66) before any test runs.
# Retry only that startup failure; every attempt is logged.
cd /root/bench/spsc-latency
echo "CMD: TSAN_OPTIONS=halt_on_error=1 taskset -c 2-7 ./build-tsan-clang/spsc_tests --items 100000000 --timeout-seconds 1200"
echo "compiler: $(clang++ --version | head -1); vm.mmap_rnd_bits=$(cat /proc/sys/vm/mmap_rnd_bits)"
for i in $(seq 1 30); do
  echo "=== attempt $i start $(date -u +%FT%TZ)"
  TSAN_OPTIONS=halt_on_error=1 taskset -c 2-7 ./build-tsan-clang/spsc_tests --items 100000000 --timeout-seconds 1200 > /tmp/tsan_attempt.txt 2>&1
  rc=$?
  cat /tmp/tsan_attempt.txt
  echo "=== attempt $i EXIT=$rc end $(date -u +%FT%TZ)"
  if [ $rc -ne 66 ] || grep -q PASS /tmp/tsan_attempt.txt; then break; fi
done
