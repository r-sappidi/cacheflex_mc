#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "warning: Neoverse N2 runner is deprecated; using cacheflex_micro Cortex config instead" >&2
exec "$SCRIPT_DIR/run_acl_fp16_w1w7_optimal_cortex_cacheflex_micro.sh" "$@"
