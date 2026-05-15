#!/bin/bash

set -ex

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <ior path> <log level> [ranks=1]"
    exit 1
fi

IOR_BIN=$1
LOG_LEVEL=$2
NRANKS=${3:-1}

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
cp "$IOR_BIN" /tmp/dfs-prototype/ior
sudo "$ROOT/scripts/mount-hugepage.sh"

start_time=$(date +%s.%N)
sudo mpirun --allow-run-as-root \
    --bind-to core --cpu-set "$CLIENT_CPUS" \
    -n "$NRANKS" \
    "$SCRIPTS_DIR/ior-runner.sh" "$LOG_LEVEL"
end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "ior use: %.2f s\n" "$elapsed"

sleep 5
sudo "$ROOT/scripts/distributed/kill-test-servers.sh"
sudo "$ROOT/scripts/insert_ns_delay/cpu-freq-stable.sh" disable
