#!/bin/bash

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <log level> <ori path> <des path>"
    echo "Error: Missing required parameters."
    exit 1
fi

if   [ -n "$OMPI_COMM_WORLD_RANK" ]; then
  RANK=$OMPI_COMM_WORLD_RANK
elif [ -n "$PMI_RANK"            ]; then
  RANK=$PMI_RANK
else
  RANK=0
fi
TARGET="${3}${RANK}"

export LD_PRELOAD=/tmp/dfs-prototype/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=/tmp/dfs-prototype/client.toml
export SPDLOG_LEVEL=$1 # This only affects client
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-prototype-client-cp.log

python3 /tmp/dfs-prototype/data_load.py "$2" "$TARGET" --mode serial