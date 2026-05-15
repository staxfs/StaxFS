#!/bin/bash

set -euo pipefail

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"

. $SCRIPTS_DIR/functions.sh

export LOG_LEVEL=${LOG_LEVEL:-"info"}
echo "Set LOG_LEVEL: $LOG_LEVEL"

# 1: role 2: count 3: server_bin_path 4: conf_path
start_servers $1 $2 $3 $4