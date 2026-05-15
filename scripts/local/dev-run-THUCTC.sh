#!/bin/bash

set -ex

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <THUCTC path> <THUCTC data path> <log level> [ranks=30]"
    exit 1
fi

THUCTC=$1
DATA_PATH=$2
LOG_LEVEL=$3
NRANKS=${4:-30}

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
ROOT="$(readlink -f "$SCRIPTS_DIR/../..")"
CONF="$ROOT/conf/local"
CLIENT_CPUS="64-127"

cmake --build "$ROOT/build"

# Compile THUCTC sources before staging the bin/ dir.
mkdir -p "$THUCTC/bin"
find "$THUCTC/src" -name "*.java" > "$THUCTC/sources.txt"
javac -cp "$THUCTC/lib/*" -d "$THUCTC/bin" @"$THUCTC/sources.txt"

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
cp -r "$THUCTC/bin" /tmp/dfs-prototype/bin
cp -r "$THUCTC/lib" /tmp/dfs-prototype/lib
cp -r "$THUCTC/my_novel_model" /tmp/dfs-prototype/my_novel_model
sudo "$ROOT/scripts/mount-hugepage.sh"

# Bulk-load corpus into /dfs/THUCNews before timed phase.
sudo "$ROOT/scripts/distributed/THUCTC-data-load.sh" "$CONF" "$LOG_LEVEL" "$DATA_PATH" "/dfs/THUCNews"

start_time=$(date +%s.%N)
sudo mpirun --allow-run-as-root \
    --bind-to core --cpu-set "$CLIENT_CPUS" \
    -n "$NRANKS" \
    "$ROOT/scripts/distributed/THUCTC-runner.sh" "$LOG_LEVEL" "/dfs/THUCNews"
end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "THUCTC use: %.2f s\n" "$elapsed"

filecount=$(find "$DATA_PATH" | wc -l)
io_summary=$(echo "$filecount * $NRANKS * 4" | bc -l) # 4 ops per file
ops_per_sec=$(echo "$io_summary / $elapsed" | bc -l)
printf "IO Summary: %.0f ops %.3f ops/s\n" "$io_summary" "$ops_per_sec"

sleep 5
sudo "$ROOT/scripts/distributed/kill-test-servers.sh"
sudo "$ROOT/scripts/insert_ns_delay/cpu-freq-stable.sh" disable
