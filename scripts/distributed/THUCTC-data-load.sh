#!/bin/bash

if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <conf path> <log level> <THUCTC data path> <target directory>"
    echo "Error: Missing required parameters."
    exit 1
fi

config="$1/client.toml"
sed -i '0,/port = 31850/{s/31850/31860/}' "$config"
ip=$(awk '
  /^\s*\[meta\.0\]/     { in_m0=1; next }
  in_m0 && /^[[:space:]]*host[[:space:]]*=/ {
    match($0, /"([^"]+)"/, m)
    print m[1]
    exit
  }
' "$config")
sed -i "0,/host = \"localhost\"/{s/localhost/$ip/}" "$config"
trap "\
  LD_PRELOAD=; \
  sed -i '0,/port = 31860/{s/31860/31850/}' \"$config\"; \
  sed -i '0,/host = \"$ip\"/{s/$ip/localhost/}' \"$config\"\
" EXIT

export LD_PRELOAD=./build/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=$config
export SPDLOG_LEVEL=$2 # This only affects client

export DFS_LOG_FILENAME=dfs-prototype-client-THUCTC.log
export DFS_CLIENT_WAIT_ATTACH_SEC=0

numactl -C 0-31 -- python3 -u ./scripts/distributed/data_load.py "$3" "$4" --mode parallel
