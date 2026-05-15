#!/bin/bash

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <log level>"
    exit 1
fi

export LD_PRELOAD=/tmp/dfs-prototype/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=/tmp/dfs-prototype/client.toml
export SPDLOG_LEVEL=$1 # This only affects client
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-prototype-client-ior.log

/tmp/dfs-prototype/ior \
    -a POSIX -w -r -i 5 -t 1k -b 16m -s 4 -o /dfs/ior
