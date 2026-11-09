#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -s) != Linux || $# != 2 || ! $1 =~ ^[0-9]+$ || ! $2 =~ ^[0-9]+$ || $1 == "$2" ]]; then
  echo "usage (Linux): $0 CPU_A CPU_B" >&2
  echo "Choose distinct allowed CPUs using lscpu -e and /proc/self/status." >&2
  exit 2
fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
host=$(hostname -s)
host=${host//[^a-zA-Z0-9_.-]/_}
output="results/linux-$host"
if [[ -e $output ]]; then
  echo "$output already exists; archive it before rerunning." >&2
  exit 2
fi
mkdir -p "$output"
{
  date -u '+UTC %Y-%m-%dT%H:%M:%SZ'
  uname -a
  lscpu
  sed -n '/Cpus_allowed_list/p' /proc/self/status
} > "$output/machine.txt"
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DSPSC_TSAN=OFF \
  -DSPSC_NATIVE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$output/build.txt" 2>&1
cmake --build build-linux --parallel "${JOBS:-4}" >> "$output/build.txt" 2>&1
cp build-linux/compile_commands.json "$output/compile_commands.json"
sha256sum CMakeLists.txt Makefile include/spsc/*.hpp bench/*.hpp bench/main.cpp tests/tests.cpp \
  scripts/run_linux.sh > "$output/source-sha256.txt"
./build-linux/spsc_tests --items "${STRESS_ITEMS:-100000000}" > "$output/tests.txt" 2>&1
./build-linux/spsc_bench --cpu-a "$1" --cpu-b "$2" --qos default \
  --capacities "${CAPACITIES:-2,8,64,1024,65536}" --items "${ITEMS:-10000000}" \
  --samples "${SAMPLES:-1000000}" --warmup "${WARMUP:-100000}" \
  --repeats "${REPEATS:-3}" --seed 20261108 --output "$output" 2>&1 | tee "$output/run.txt"
echo "Results: $root/$output"
