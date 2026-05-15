#!/bin/bash

set -ex

if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <conf path> <log level> <filebench path> <filebench workload file> <filebench workload count>"
    echo "Error: Missing required parameters."
    exit 1
fi

cmake --build build

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh enable
cat /proc/cpuinfo | grep -m1 "MHz"

meta_num=$(grep -A1 "^\[meta\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
data_num=$(grep -A1 "^\[data\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
sudo ./scripts/distributed/setup-test-servers.sh $1 $2 $meta_num $data_num

echo "$1/client_list"

counter=1
for ip in $(cat "$1/client_list"); do
    if [[ -n "$ip" ]]; then
        echo "Running command on $ip"
        # ssh $ip "cd ~/dfs-prototype && cmake --build build"
        
        ssh $ip "mkdir -p /tmp/dfs-prototype"
        ssh $ip "sudo rm -rf /dev/shm/.dfs-client.ranks"
        scp $1/client.toml $ip:/tmp/dfs-prototype/client.toml
        ssh $ip "sed -i 's/host = \"localhost\"/host = \"$ip\"/' /tmp/dfs-prototype/client.toml"
        ssh $ip "cp ~/dfs-prototype/build/libdfs-hook.so /tmp/dfs-prototype/libdfs-hook.so"
        scp ./scripts/distributed/filebench-runner.sh $ip:/tmp/dfs-prototype/filebench-runner.sh
        for i in $(seq 1 $5); do
            remote_path="/tmp/dfs-prototype/filebench-workload${i}"
            scp "$4" "$ip:$remote_path"
            ssh "$ip" "sed -i 's|set \$dir=/dfs|set \$dir=/dfs/filebench${counter}|' $remote_path"
            counter=$((counter + 1))
        done
        scp ./scripts/mount-hugepage.sh $ip:/tmp/dfs-prototype/mount-hugepage.sh
        scp ./scripts/clear-hugepage.sh $ip:/tmp/dfs-prototype/clear-hugepage.sh
        ssh $ip "cp $3 /tmp/dfs-prototype/filebench"

        ssh $ip "sudo /tmp/dfs-prototype/mount-hugepage.sh"
    fi
done

echo "client_list2 path: $1/client_list2"

for ip in $(cat "$1/client_list2"); do
    if [[ -n "$ip" ]]; then
        echo "Running command on: $ip"
        for i in $(seq $(($5 + 1)) $(($5 * 2))); do
            remote_path="/tmp/dfs-prototype/filebench-workload${i}"
            scp "$4" "$ip:$remote_path"
            ssh "$ip" "sed -i 's|set \$dir=/dfs|set \$dir=/dfs/filebench${counter}|' $remote_path"
            counter=$((counter + 1))
        done
    fi
done

PIDS=()
start_time=$(date +%s.%N)
for ip in $(cat "$1/client_list"); do
    NUMA_NODES=$(ssh $ip "ls -d /sys/devices/system/node/node* | wc -l")
    for i in $(seq 1 $5); do
        numa_node=$(( (i-1) % NUMA_NODES ))
        remote_path="/tmp/dfs-prototype/filebench-workload${i}"
        ssh $ip "sudo numactl --cpunodebind=$numa_node --membind=$numa_node /tmp/dfs-prototype/filebench-runner.sh $2 $remote_path" &
        PIDS+=($!)
    done
done
for ip in $(cat "$1/client_list2"); do
    NUMA_NODES=$(ssh $ip "ls -d /sys/devices/system/node/node* | wc -l")
    for i in $(seq $(($5 + 1)) $(($5 * 2))); do
        numa_node=$(( (i-1) % NUMA_NODES ))
        remote_path="/tmp/dfs-prototype/filebench-workload${i}"
        ssh $ip "sudo numactl --cpunodebind=$numa_node --membind=$numa_node /tmp/dfs-prototype/filebench-runner.sh $2 $remote_path" &
        PIDS+=($!)
    done
done
for pid in "${PIDS[@]}"; do
    wait $pid
done

end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "filebench use: %.2f s\n" "$elapsed"

LOG_FILE=$(readlink /proc/$$/fd/1 2>/dev/null)
if [[ -n "$LOG_FILE" && -f "$LOG_FILE" ]]; then
    python3 ./scripts/distributed/organize-filebench-result.py "$LOG_FILE"
fi

for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo /tmp/dfs-prototype/clear-hugepage.sh"
    ssh $ip "rm -rf /tmp/dfs-prototype"
done

sudo ./scripts/distributed/kill-test-servers.sh

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh disable