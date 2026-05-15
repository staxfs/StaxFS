#!/bin/bash
# Pin to Node 1 CPU 32 for consistent measurement with CXL benchmarks.
# Requires sudo for turbo boost / c-state control.

./cpu-freq-stable.sh enable

make

sleep 1

echo "=== CPU freq before ==="
cat /sys/devices/system/cpu/cpu32/cpufreq/scaling_cur_freq

taskset -c 32 ./latency

echo "=== CPU freq after ==="
cat /sys/devices/system/cpu/cpu32/cpufreq/scaling_cur_freq

make clean

# ./cpu-freq-stable.sh disable
