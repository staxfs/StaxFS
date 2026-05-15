#!/bin/bash

set -euo pipefail

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"
exec "$SCRIPTS_DIR/../distributed/kill-test-servers.sh"
