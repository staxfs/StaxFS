#!/bin/bash

set -ex

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <filebench path> <workload file> <log level> [parallel workloads=1]"
    exit 1
fi

FILEBENCH_BIN=$1
WORKLOAD_FILE=$2
LOG_LEVEL=$3
NWORKLOADS=${4:-1}

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
ROOT="$(readlink -f "$SCRIPTS_DIR/../..")"
CONF="$ROOT/conf/local"

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
cp "$FILEBENCH_BIN" /tmp/dfs-prototype/filebench
sudo "$ROOT/scripts/mount-hugepage.sh"

# Stage one workload file per parallel run, each rooted at /dfs/filebenchN.
for i in $(seq 1 "$NWORKLOADS"); do
    remote_path="/tmp/dfs-prototype/filebench-workload${i}"
    cp "$WORKLOAD_FILE" "$remote_path"
    sed -i "s|set \$dir=/dfs|set \$dir=/dfs/filebench${i}|" "$remote_path"
done

PIDS=()
start_time=$(date +%s.%N)
NUMA_NODES=$(ls -d /sys/devices/system/node/node* | wc -l)
for i in $(seq 1 "$NWORKLOADS"); do
    numa_node=$(( (i - 1) % NUMA_NODES ))
    remote_path="/tmp/dfs-prototype/filebench-workload${i}"
    sudo numactl --cpunodebind="$numa_node" --membind="$numa_node" \
        "$ROOT/scripts/distributed/filebench-runner.sh" "$LOG_LEVEL" "$remote_path" &
    PIDS+=($!)
done
for pid in "${PIDS[@]}"; do
    wait "$pid"
done
end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "filebench use: %.2f s\n" "$elapsed"

sleep 5
sudo "$ROOT/scripts/distributed/kill-test-servers.sh"
sudo rm -rf /tmp/filebench-shm*
sudo "$ROOT/scripts/insert_ns_delay/cpu-freq-stable.sh" disable
