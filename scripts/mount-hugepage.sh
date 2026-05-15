#!/bin/bash

set -euo pipefail

mkdir -p /dev/hugepages
mountpoint -q /dev/hugepages || mount -t hugetlbfs nodev /dev/hugepages
PAGHS=2048

for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do
    CURRENT_PAGES=$(cat $node)
    NEW_PAGES=$(($CURRENT_PAGES + $PAGHS))
    echo "$NEW_PAGES" > $node
done

for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages; do
    CURRENT_PAGES=$(cat $node)
    if [ "$CURRENT_PAGES" -lt $PAGHS ]; then
        echo "Warning: Not enough free hugepages available on $(dirname $node)"
        # exit 1
    fi
done

echo "All nodes have at least $PAGHS free hugepages."