#!/bin/bash

set -euo pipefail

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"

$SCRIPTS_DIR/../kill-servers.sh "data" 9
$SCRIPTS_DIR/../kill-servers.sh "meta" 9

rm -rf /tmp/dfs-prototype/*
rm -rf /dev/hugepages/cxl_memory
rm -rf /dev/hugepages/gim_memory*
rm -rf /sharenvme/usershome/hyx/dfs_ssd/*
rm -rf /dev/shm/.dfs-client.ranks
./scripts/clear-hugepage.sh
