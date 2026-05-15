#!/bin/bash

set -euo pipefail

SCRIPTS_DIR="$(dirname "$(readlink -f "$0")")"

. $SCRIPTS_DIR/functions.sh

kill_servers $1 $2
