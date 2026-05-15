#!/bin/bash

set -ex

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <source data path> <log level> [ranks=20]"
    exit 1
fi

if [ ! -e "$1" ]; then
    echo "Error: There is no such directory: $1"
    exit 1
fi

DATA_PATH=$1
LOG_LEVEL=$2
NRANKS=${3:-20}

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
ROOT="$(readlink -f "$SCRIPTS_DIR/../..")"
CONF="$ROOT/conf/local"
CLIENT_CPUS="64-127"

cmake --build "$ROOT/build"

sudo "$ROOT/scripts/insert_ns_delay/cpu-freq-stable.sh" enable
cat /proc/cpuinfo | grep -m1 "MHz"

meta_num=$(grep -A1 "^\[meta\]" "$CONF/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
data_num=$(grep -A1 "^\[data\]" "$CONF/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
sudo "$ROOT/scripts/distributed/setup-test-servers.sh" "$CONF" "$LOG_LEVEL" "$meta_num" "$data_num"

sudo rm -f /dev/shm/.dfs-client.ranks
mkdir -p /tmp/dfs-prototype
cp "$CONF/client.toml" /tmp/dfs-prototype/client.toml
cp "$ROOT/build/libdfs-hook.so" /tmp/dfs-prototype/libdfs-hook.so
cp "$ROOT/scripts/distributed/data_load.py" /tmp/dfs-prototype/data_load.py
sudo "$ROOT/scripts/mount-hugepage.sh"

# Bulk-load source tree into /dfs/THUCNews so the timed cp below has data to copy.
sudo "$ROOT/scripts/distributed/THUCTC-data-load.sh" "$CONF" "$LOG_LEVEL" "$DATA_PATH" "/dfs/THUCNews"

start_time=$(date +%s.%N)
sudo mpirun --allow-run-as-root \
    --bind-to core --cpu-set "$CLIENT_CPUS" \
    -n "$NRANKS" \
    "$ROOT/scripts/distributed/cp-runner.sh" "$LOG_LEVEL" "/dfs/THUCNews" "/dfs/dir"
end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "cp use: %.2f s\n" "$elapsed"

filecount=$(find "$DATA_PATH" | wc -l)
io_summary=$(echo "$filecount * $NRANKS * 5" | bc -l) # 5 ops per file copy
ops_per_sec=$(echo "$io_summary / $elapsed" | bc -l)
printf "IO Summary: %.0f ops %.3f ops/s\n" "$io_summary" "$ops_per_sec"

sleep 5
sudo "$ROOT/scripts/distributed/kill-test-servers.sh"
sudo "$ROOT/scripts/insert_ns_delay/cpu-freq-stable.sh" disable
