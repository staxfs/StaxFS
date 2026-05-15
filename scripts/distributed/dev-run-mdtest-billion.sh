#!/bin/bash

set -ex

if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <conf path> <log level> <mdtest path> <mdtest workload count> <num rounds>"
    echo "Error: Missing required parameters."
    exit 1
fi

CONF_PATH=$1
LOG_LEVEL=$2
MDTEST_PATH=$3
WORKLOAD_COUNT=$4
NUM_ROUNDS=$5

cmake --build build

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh enable
cat /proc/cpuinfo | grep -m1 "MHz"

meta_num=$(grep -A1 "^\[meta\]" "$CONF_PATH/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
data_num=$(grep -A1 "^\[data\]" "$CONF_PATH/client.toml" | grep -E "^num" | sed 's/[^0-9]*//g')
sudo ./scripts/distributed/setup-test-servers.sh "$CONF_PATH" "$LOG_LEVEL" "$meta_num" "$data_num"

for ip in $(cat "$1/client_list"); do
    if [[ -n "$ip" ]]; then
        ssh $ip "mkdir -p /tmp/dfs-prototype"
        ssh $ip "sudo rm -rf /dev/shm/.dfs-client.ranks"
        scp "$CONF_PATH/client.toml" $ip:/tmp/dfs-prototype/client.toml
        ssh $ip "sed -i 's/host = \"localhost\"/host = \"$ip\"/' /tmp/dfs-prototype/client.toml"
        ssh $ip "cp ~/dfs-prototype/build/libdfs-hook.so /tmp/dfs-prototype/libdfs-hook.so"
        scp ./scripts/mount-hugepage.sh $ip:/tmp/dfs-prototype/mount-hugepage.sh
        scp ./scripts/clear-hugepage.sh $ip:/tmp/dfs-prototype/clear-hugepage.sh
        ssh $ip "cp $MDTEST_PATH /tmp/dfs-prototype/mdtest"
        ssh $ip "sudo /tmp/dfs-prototype/mount-hugepage.sh"
    fi
done

set +e
counter=0

for (( round=1; round<=NUM_ROUNDS; round++ )); do
    echo "=== Round $round start ==="
    LOG_FILE="mdtest_round_${round}.log"
    > "$LOG_FILE"

    for ip in $(cat "$1/client_list"); do
        if [[ -n "$ip" ]]; then
            scp ./scripts/distributed/mdtest-billion-runner.sh $ip:/tmp/dfs-prototype/mdtest-runner.sh
            ssh $ip "sed -i 's|/dfs/mdtest|/dfs/mdtest${counter}|' /tmp/dfs-prototype/mdtest-runner.sh"
            counter=$((counter + 1))
        fi
    done
    for ip in $(cat "$1/client_list2"); do
        if [[ -n "$ip" ]]; then
            scp ./scripts/distributed/mdtest-billion-runner.sh $ip:/tmp/dfs-prototype/mdtest-runner2.sh
            ssh $ip "sed -i 's|/dfs/mdtest|/dfs/mdtest${counter}|' /tmp/dfs-prototype/mdtest-runner2.sh"
            counter=$((counter + 1))
        fi
    done

    PIDS=()
    start_time=$(date +%s.%N)

    for ip in $(cat "$1/client_list"); do
        ssh $ip "sudo mpirun --allow-run-as-root --oversubscribe --cpu-set 0-63 -n $WORKLOAD_COUNT /tmp/dfs-prototype/mdtest-runner.sh $LOG_LEVEL 1" >> "$LOG_FILE" 2>&1 &
        PIDS+=($!)
    done
    for ip in $(cat "$1/client_list2"); do
        ssh $ip "sudo mpirun --allow-run-as-root --oversubscribe --cpu-set 64-127 -n $WORKLOAD_COUNT /tmp/dfs-prototype/mdtest-runner2.sh $LOG_LEVEL 1" >> "$LOG_FILE" 2>&1 &
        PIDS+=($!)
    done

    round_failed=0
    for pid in "${PIDS[@]}"; do
        wait $pid
        if [ $? -ne 0 ]; then
            round_failed=1
        fi
    done

    end_time=$(date +%s.%N)
    elapsed=$(echo "$end_time - $start_time" | bc -l)
    printf "Round %d elapsed: %.2f s\n" "$round" "$elapsed"

    if [ -f "$LOG_FILE" ]; then
        python3 ./scripts/distributed/organize-mdtest-result.py "$LOG_FILE" >> "$LOG_FILE"
    fi

    if [ $round_failed -eq 1 ]; then
        echo "Round $round has failures, stopping further rounds."
        break
    fi
done

for ip in $(cat "$1/client_list"); do
    ssh $ip "sudo /tmp/dfs-prototype/clear-hugepage.sh"
    ssh $ip "rm -rf /tmp/dfs-prototype"
done

sudo ./scripts/distributed/kill-test-servers.sh

sudo ./scripts/insert_ns_delay/cpu-freq-stable.sh disable