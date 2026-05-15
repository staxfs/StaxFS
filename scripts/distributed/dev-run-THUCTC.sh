#!/bin/bash

set -ex

if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <conf path> <log level> <THUCTC path> <THUCTC data path> <THUCTC workload count>"
    echo "Error: Missing required parameters."
    exit 1
fi

cmake --build build

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh enable
cat /proc/cpuinfo | grep -m1 "MHz"

meta_num=$(grep -A1 "^\[meta\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
data_num=$(grep -A1 "^\[data\]" "$1/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
sudo ./scripts/distributed/setup-test-servers.sh $1 $2 $meta_num $data_num

# load from local -> dfs
sudo ./scripts/mount-hugepage.sh
sudo ./scripts/distributed/THUCTC-data-load.sh $1 $2 $4 "/dfs/THUCNews"

echo "$1/client_list"

for ip in $(cat "$1/client_list"); do
    if [[ -n "$ip" ]]; then
        echo "Running command on $ip"
        # ssh $ip "cd ~/dfs-prototype && cmake --build build"

        ssh $ip "mkdir -p /tmp/dfs-prototype"
        ssh $ip "sudo rm -rf /dev/shm/.dfs-client.ranks"
        scp $1/client.toml $ip:/tmp/dfs-prototype/client.toml
        ssh $ip "sed -i 's/host = \"localhost\"/host = \"$ip\"/' /tmp/dfs-prototype/client.toml"
        ssh $ip "cp ~/dfs-prototype/build/libdfs-hook.so /tmp/dfs-prototype/libdfs-hook.so"
        scp ./scripts/distributed/THUCTC-runner.sh $ip:/tmp/dfs-prototype/THUCTC-runner.sh
        ssh $ip "cp -r $3/bin /tmp/dfs-prototype/bin"
        ssh $ip "cp -r $3/lib /tmp/dfs-prototype/lib"
        ssh $ip "cp -r $3/my_novel_model /tmp/dfs-prototype/my_novel_model"
        scp ./scripts/mount-hugepage.sh $ip:/tmp/dfs-prototype/mount-hugepage.sh
        scp ./scripts/clear-hugepage.sh $ip:/tmp/dfs-prototype/clear-hugepage.sh

        ssh $ip "sudo /tmp/dfs-prototype/mount-hugepage.sh"
    fi
done

PIDS=()
counter=1
start_time=$(date +%s.%N)
for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo mpirun --allow-run-as-root -n $5 /tmp/dfs-prototype/THUCTC-runner.sh $2 /dfs/THUCNews" &
    counter=$((counter + 1))
    PIDS+=($!)
done
for ip in $(cat "$1/client_list2"); do
    ssh $ip "sudo mpirun --allow-run-as-root -n $5 /tmp/dfs-prototype/THUCTC-runner.sh $2 /dfs/THUCNews" &
    counter=$((counter + 1))
    PIDS+=($!)
done
for pid in "${PIDS[@]}"; do
    wait $pid
done
end_time=$(date +%s.%N)
elapsed=$(echo "$end_time - $start_time" | bc -l)
printf "THUCTC use: %.6f s\n" "$elapsed"

filecount=$(find "$4" | wc -l)
io_summery=$(echo "$filecount * ($counter - 1) * $5 * 4" | bc -l) # 4 ops per file calculation
ops_per_sec=$(echo "$io_summery / $elapsed" | bc -l)
printf "IO Summary: %.0f ops %.3f ops/s\n" "$io_summery" "$ops_per_sec"

for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo /tmp/dfs-prototype/clear-hugepage.sh"
    ssh $ip "rm -rf /tmp/dfs-prototype"
done

sudo ./scripts/distributed/kill-test-servers.sh
sudo ./scripts/clear-hugepage.sh

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh disable
