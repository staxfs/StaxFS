#!/bin/bash

set -euo pipefail

# Thin wrapper around scripts/distributed/setup-test-servers.sh for the local
# (single-machine) topology: 3 meta + 3 data described by ./conf/local/.
#
# Usage: $0 [LOG_LEVEL]   (default: info)

LOG_LEVEL=${1:-info}
SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
ROOT="$(readlink -f "$SCRIPTS_DIR/../..")"

exec "$ROOT/scripts/distributed/setup-test-servers.sh" \
    "$ROOT/conf/local" "$LOG_LEVEL" 3 3
