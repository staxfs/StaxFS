#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <log level> <data path>"
    echo "Error: Missing required parameters."
    exit 1
fi

export LD_PRELOAD=/tmp/dfs-prototype/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=/tmp/dfs-prototype/client.toml
export SPDLOG_LEVEL=$1 # This only affects client
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-prototype-client-THUCTC.log

java -cp "/tmp/dfs-prototype/bin:/tmp/dfs-prototype/lib/*" Demo $2 /tmp/dfs-prototype/my_novel_model 1.0