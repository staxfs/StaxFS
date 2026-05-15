#!/bin/bash

set -euo pipefail

mkdir -p /dev/hugepages
mountpoint -q /dev/hugepages || mount -t hugetlbfs nodev /dev/hugepages
for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do
    echo 0 | sudo tee "$node" > /dev/null
done
