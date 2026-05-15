#!/bin/bash

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <log level> <mdtest repeat count>"
    echo "Error: Missing required parameters."
    exit 1
fi

export LD_PRELOAD=/tmp/dfs-prototype/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=/tmp/dfs-prototype/client.toml
export SPDLOG_LEVEL=$1 # This only affects client
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-prototype-client-mdtest.log

/tmp/dfs-prototype/mdtest -a POSIX -d /dfs/mdtest -I 4000 -n 40000 -i $2 -C -T -F
