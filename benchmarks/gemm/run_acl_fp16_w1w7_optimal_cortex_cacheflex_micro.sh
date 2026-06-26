#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/run_acl_fp16_w1w7_optimal_cacheflex_micro.sh" "$@"
