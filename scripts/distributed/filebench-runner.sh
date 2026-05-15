#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <log level> <filebench workload file>"
    echo "Error: Missing required parameters."
    exit 1
fi

# filebench needs this setting: https://github.com/filebench/filebench/issues/112
if [[ `cat /proc/sys/kernel/randomize_va_space` -ne 0 ]]; then
    echo 0 > /proc/sys/kernel/randomize_va_space
fi

export LD_PRELOAD=/tmp/dfs-prototype/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=/tmp/dfs-prototype/client.toml
export SPDLOG_LEVEL=$1 # This only affects client
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-prototype-client-filebench.log

/tmp/dfs-prototype/filebench -f $2