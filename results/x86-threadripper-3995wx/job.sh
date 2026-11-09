#!/bin/bash
# Master job: spsc-latency then orderbook-latency, all pinned to CPUs 2-7.
set -uo pipefail
B=/root/bench
P="taskset -c 2-7"
step() { echo "== $(date -u +%FT%TZ) $*"; }
# background sampler: busy% of cores 2-7 and their SMT siblings, 5 s window every 30 s
( while true; do echo "--- $(date -u +%FT%TZ)"; bash /root/corebusy.sh 5 2 3 4 5 6 7 66 67 68 69 70 71; sleep 25; done ) > $B/corebusy.log 2>&1 &
SAMPLER=$!
trap 'kill $SAMPLER 2>/dev/null' EXIT

########## spsc-latency
cd $B/spsc-latency
X=$B/spsc-extra; mkdir -p $X
{
  date -u '+UTC %Y-%m-%dT%H:%M:%SZ'; uname -a; cat /etc/os-release | head -4
  echo "c++ -> $(readlink -f $(command -v c++))"; c++ --version; cmake --version | head -1
  lscpu; sed -n '/Cpus_allowed_list/p' /proc/self/status
  echo "governor cpu2: $(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null)"
  echo "L3 shared with cpu2: $(cat /sys/devices/system/cpu/cpu2/cache/index3/shared_cpu_list)"
  echo "SMT siblings: cpu2=$(cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list) cpu3=$(cat /sys/devices/system/cpu/cpu3/topology/thread_siblings_list)"
  echo "loadavg (host-wide, shared rented box): $(cat /proc/loadavg)"
  bash /root/corebusy.sh 5 2 3 4 5 6 7 66 67 68 69 70 71
} > $X/machine-extra.txt 2>&1
step "spsc tsan build"
$P cmake -S . -B build-tsan -DSPSC_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > $X/build-tsan.txt 2>&1 && \
$P cmake --build build-tsan -j 4 >> $X/build-tsan.txt 2>&1; echo "EXIT=$?" >> $X/build-tsan.txt
cp build-tsan/compile_commands.json $X/compile_commands-tsan.json
step "spsc tsan 100M"
( echo "CMD: TSAN_OPTIONS=halt_on_error=1 taskset -c 2-7 setarch -R ./build-tsan/spsc_tests --items 100000000 --timeout-seconds 1200"
  TSAN_OPTIONS=halt_on_error=1 $P setarch -R ./build-tsan/spsc_tests --items 100000000 --timeout-seconds 1200; echo "EXIT=$?" ) > $X/tests-tsan.txt 2>&1
step "spsc normal build for ctest/capacity-one"
$P cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > $X/build-normal.txt 2>&1 && \
$P cmake --build build -j 4 >> $X/build-normal.txt 2>&1; echo "EXIT=$?" >> $X/build-normal.txt
( echo "CMD: taskset -c 2-7 ctest --test-dir build --output-on-failure"; $P ctest --test-dir build --output-on-failure; echo "EXIT=$?" ) > $X/tests-ctest.txt 2>&1
( echo "CMD: taskset -c 2-7 ./build/spsc_tests --items 1000000 --capacity 1"; $P ./build/spsc_tests --items 1000000 --capacity 1; echo "EXIT=$?" ) > $X/tests-capacity-one.txt 2>&1
step "spsc run_linux.sh 2 3"
( echo "CMD: JOBS=4 taskset -c 2-7 ./scripts/run_linux.sh 2 3"; JOBS=4 $P ./scripts/run_linux.sh 2 3; echo "EXIT=$?" ) > $X/run_linux-console.txt 2>&1
step "spsc done"

########## orderbook-latency
cd $B/orderbook-latency
Y=$B/lob-extra; mkdir -p $Y
step "lob run_linux.sh 2"
( echo "CMD: JOBS=4 taskset -c 2-7 ./scripts/run_linux.sh 2"; JOBS=4 $P ./scripts/run_linux.sh 2; echo "EXIT=$?" ) > $Y/run_linux-console.txt 2>&1
OUT=results/linux-$(hostname -s)
step "lob populated/overflow"
( echo "CMD: taskset -c 2-7 ./build-linux/lob_bench --messages 200000 --warmup 20000 --adds 60 --cancels 25 --modifies 10 --aggressive 5 --capacity 65536 --overflow 2048 --pin 2 --csv $OUT/populated.csv"
  $P ./build-linux/lob_bench --messages 200000 --warmup 20000 --adds 60 --cancels 25 --modifies 10 --aggressive 5 \
  --capacity 65536 --overflow 2048 --pin 2 --csv $OUT/populated.csv; echo "EXIT=$?" ) > $Y/populated.txt 2>&1
( echo "CMD: taskset -c 2-7 ./build-linux/lob_bench --messages 200000 --warmup 20000 --tail 50 --overflow 2048 --pin 2 --csv $OUT/overflow.csv"
  $P ./build-linux/lob_bench --messages 200000 --warmup 20000 --tail 50 --overflow 2048 --pin 2 --csv $OUT/overflow.csv; echo "EXIT=$?" ) > $Y/overflow.txt 2>&1
step "lob make test"
( echo "CMD: taskset -c 2-7 make BUILD=build-make -j 4 test"; $P make BUILD=build-make -j 4 test; echo "EXIT=$?" ) > $Y/tests-make.txt 2>&1
bash /root/corebusy.sh 5 2 3 4 5 6 7 66 67 68 69 70 71 > $Y/corebusy-end.txt 2>&1
step "ALL DONE"
