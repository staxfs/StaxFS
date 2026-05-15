#!/bin/bash

set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <conf path> <log level> <meta nums> <data nums>"
    echo "Error: Missing required parameters."
    exit 1
fi

./scripts/distributed/kill-test-servers.sh

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
rm -f $SCRIPTS_DIR/../../dfs-prototype-*
conf_path=$(readlink -f "$1")
echo "conf_path: $conf_path"

. $SCRIPTS_DIR/../functions.sh

export LOG_LEVEL=$2
echo "Set LOG_LEVEL: $LOG_LEVEL"

META_NUMS=$3

# Collect data_root_path from each meta config
declare -a META_DATA_DIRS
for i in $(seq 0 $((META_NUMS - 1))); do
    toml_file="$conf_path/meta-${i}.toml"
    META_DATA_DIRS[$i]=$(grep -Po '(?<=data_root_path = ")[^"]*' "$toml_file")
done

max_retries=3
retry_count=0
while true; do
    $SCRIPTS_DIR/../start-servers.sh "meta" $META_NUMS $SCRIPTS_DIR/../../build/dfs-prototype-server $conf_path

    # Wait for all meta servers to create their data_root_path directory
    timeout=300
    elapsed=0
    while true; do
        ready_count=0
        for i in $(seq 0 $((META_NUMS - 1))); do
            if [ -d "${META_DATA_DIRS[$i]}" ]; then
                ready_count=$((ready_count + 1))
            fi
        done
        if [ "$ready_count" -ge "$META_NUMS" ]; then
            break
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
        if [ "$elapsed" -ge "$((timeout * 2))" ]; then
            echo "Timeout waiting for meta servers ($ready_count/$META_NUMS ready)"
            break
        fi
    done
    sleep 1

    startup_ok=true
    for errfile in $SCRIPTS_DIR/../../*stderr*; do
        if [ -s "$errfile" ]; then
            echo "Startup error! Retrying..."
            startup_ok=false
            break
        fi
    done

    if $startup_ok; then
        DATA_NUMS=$4
        $SCRIPTS_DIR/../start-servers.sh "data" $DATA_NUMS $SCRIPTS_DIR/../../build/dfs-prototype-server $conf_path

        # Wait for all data servers to create their data_root_path directory
        declare -a DATA_DATA_DIRS
        for i in $(seq 0 $((DATA_NUMS - 1))); do
            toml_file="$conf_path/data-${i}.toml"
            DATA_DATA_DIRS[$i]=$(grep -Po '(?<=data_root_path = ")[^"]*' "$toml_file")
        done

        elapsed=0
        while true; do
            ready_count=0
            for i in $(seq 0 $((DATA_NUMS - 1))); do
                if [ -d "${DATA_DATA_DIRS[$i]}" ]; then
                    ready_count=$((ready_count + 1))
                fi
            done
            if [ "$ready_count" -ge "$DATA_NUMS" ]; then
                break
            fi
            sleep 0.5
            elapsed=$((elapsed + 1))
            if [ "$elapsed" -ge "$((timeout * 2))" ]; then
                echo "Timeout waiting for data servers ($ready_count/$DATA_NUMS ready)"
                break
            fi
        done

        echo "Startup OK!"
        break
    else
        retry_count=$((retry_count + 1))
        if [ "$retry_count" -ge "$max_retries" ]; then
            echo "Startup ERROR! Exiting."
            exit 1
        fi
        echo "Startup failed! Retry attempt $retry_count/$max_retries"
        ./scripts/distributed/kill-test-servers.sh
        sleep 1
        continue
    fi

    break
done
