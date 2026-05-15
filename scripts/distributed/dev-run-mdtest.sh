#!/bin/bash

set -ex

if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <conf path> <log level> <mdtest path> <mdtest workload count>"
    echo "Error: Missing required parameters."
    exit 1
fi

cmake --build build

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh enable
cat /proc/cpuinfo | grep -m1 "MHz"

meta_num=$(grep -A1 "^\[meta\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
data_num=$(grep -A1 "^\[data\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
sudo ./scripts/distributed/setup-test-servers.sh $1 $2 $meta_num $data_num

echo "client_list path: $1/client_list"
counter=0

for ip in $(cat "$1/client_list"); do
    if [[ -n "$ip" ]]; then
        echo "Running command on: $ip"
        # ssh $ip "cd ~/dfs-prototype && cmake --build build"

        ssh $ip "mkdir -p /tmp/dfs-prototype"
        ssh $ip "sudo rm -rf /dev/shm/.dfs-client.ranks"
        scp $1/client.toml $ip:/tmp/dfs-prototype/client.toml
        ssh $ip "sed -i 's/host = \"localhost\"/host = \"$ip\"/' /tmp/dfs-prototype/client.toml"
        ssh $ip "cp ~/dfs-prototype/build/libdfs-hook.so /tmp/dfs-prototype/libdfs-hook.so"
        scp ./scripts/distributed/mdtest-runner.sh $ip:/tmp/dfs-prototype/mdtest-runner.sh
        ssh $ip "sed -i 's|/dfs/mdtest|/dfs/mdtest$counter|' /tmp/dfs-prototype/mdtest-runner.sh"
        counter=$((counter + 1))
        scp ./scripts/mount-hugepage.sh $ip:/tmp/dfs-prototype/mount-hugepage.sh
        scp ./scripts/clear-hugepage.sh $ip:/tmp/dfs-prototype/clear-hugepage.sh
        ssh $ip "cp $3 /tmp/dfs-prototype/mdtest"
        ssh $ip "sudo /tmp/dfs-prototype/mount-hugepage.sh"
    fi
done

echo "client_list2 path: $1/client_list2"

for ip in $(cat "$1/client_list2"); do
    if [[ -n "$ip" ]]; then
        echo "Running command on: $ip"
        scp ./scripts/distributed/mdtest-runner.sh $ip:/tmp/dfs-prototype/mdtest-runner2.sh
        ssh $ip "sed -i 's|/dfs/mdtest|/dfs/mdtest$counter|' /tmp/dfs-prototype/mdtest-runner2.sh"
        counter=$((counter + 1))
    fi
done

PIDS=()
start_time=$(date +%s.%N)
for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo mpirun --allow-run-as-root --cpu-set 0-63 -n $4 /tmp/dfs-prototype/mdtest-runner.sh $2 1" &
    PIDS+=($!)
done
for ip in $(cat "$1/client_list2"); do
    ssh $ip "sudo mpirun --allow-run-as-root --cpu-set 64-127 -n $4 /tmp/dfs-prototype/mdtest-runner2.sh $2 1" &
    PIDS+=($!)
done
for pid in "${PIDS[@]}"; do
    wait $pid
done

end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "mdtest use: %.2f s\n" "$elapsed"

LOG_FILE=$(readlink /proc/$$/fd/1 2>/dev/null)
if [[ -n "$LOG_FILE" && -f "$LOG_FILE" ]]; then
    python3 ./scripts/distributed/organize-mdtest-result.py "$LOG_FILE"
fi

for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo /tmp/dfs-prototype/clear-hugepage.sh"
    ssh $ip "rm -rf /tmp/dfs-prototype"
done

sleep 5
sudo ./scripts/distributed/kill-test-servers.sh

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh disable