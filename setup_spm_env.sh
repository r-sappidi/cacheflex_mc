#!/usr/bin/env bash
# Source this before building/running the ported SPM benchmark scaffolding.

_SETUP_SRC="${BASH_SOURCE[0]:-$0}"
export CACHEFLEX_MC_ROOT="$(cd "$(dirname "$_SETUP_SRC")" && pwd)"
unset _SETUP_SRC

export GEM5_ROOT="${GEM5_ROOT:-$CACHEFLEX_MC_ROOT/gem5}"
export GEM5_SPM="${GEM5_SPM:-$GEM5_ROOT/build/ARM_MESI_Three_Level_SPM/gem5.opt}"
export GEM5_SE="${GEM5_SE:-$GEM5_ROOT/configs/deprecated/example/se.py}"
export SPM_COMPILER="${SPM_COMPILER:-$CACHEFLEX_MC_ROOT/spm_tools/spm_compiler.py}"
export M5_INCLUDE="${M5_INCLUDE:-$GEM5_ROOT/include}"

# This host's /usr/bin/python3 is Python 3.10, while its alternatives-managed
# python3-config points at Python 3.6.  gem5 must be built against the config
# helper matching the interpreter that runs SCons.
if [ -z "${PYTHON_CONFIG:-}" ] && [ -x /usr/local/bin/python3.10-config ]; then
    export PYTHON_CONFIG=/usr/local/bin/python3.10-config
fi

# Managed workspaces expose $HOME read-only, so ccache cannot use its default
# ~/.ccache directory during a gem5 build.
if [ -z "${CCACHE_DIR:-}" ] && [ ! -w "${HOME:-/nonexistent}" ]; then
    export CCACHE_DIR="${TMPDIR:-/tmp}/cacheflex-ccache"
fi
if [ -z "${M5OP_OBJ:-}" ]; then
    if [ -f "$GEM5_ROOT/util/m5/build/arm64/out/m5op.o" ]; then
        export M5OP_OBJ="$GEM5_ROOT/util/m5/build/arm64/out/m5op.o"
    else
        export M5OP_OBJ="$GEM5_ROOT/util/m5/build/arm64/abi/arm64/m5op.o"
    fi
fi

if [ -z "${CROSS_CXX:-}" ]; then
    if [ -x "$CACHEFLEX_MC_ROOT/tools/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++" ]; then
        export CROSS_CXX="$CACHEFLEX_MC_ROOT/tools/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++"
    elif [ -x "$CACHEFLEX_MC_ROOT/tools/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++" ]; then
        export CROSS_CXX="$CACHEFLEX_MC_ROOT/tools/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++"
    elif [ -x "$CACHEFLEX_MC_ROOT/../cacheflex_micro/tools/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++" ]; then
        export CROSS_CXX="$CACHEFLEX_MC_ROOT/../cacheflex_micro/tools/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++"
    elif [ -x "$CACHEFLEX_MC_ROOT/../AraXL_fpga/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++" ]; then
        export CROSS_CXX="$CACHEFLEX_MC_ROOT/../AraXL_fpga/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++"
    elif command -v aarch64-none-linux-gnu-g++ >/dev/null 2>&1; then
        export CROSS_CXX=aarch64-none-linux-gnu-g++
    elif command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        export CROSS_CXX=aarch64-linux-gnu-g++
    else
        export CROSS_CXX=aarch64-none-linux-gnu-g++
    fi
fi

echo "CACHEFLEX_MC_ROOT=$CACHEFLEX_MC_ROOT"
[ -x "$GEM5_SPM" ] && echo "  GEM5_SPM     : OK" || echo "  GEM5_SPM     : missing; build gem5/build/ARM_MESI_Three_Level_SPM/gem5.opt"
[ -f "$SPM_COMPILER" ] && echo "  SPM_COMPILER : OK" || echo "  SPM_COMPILER : missing"
[ -f "$M5OP_OBJ" ] && echo "  m5op.o       : OK" || echo "  m5op.o       : missing; build gem5/util/m5 arm64 m5op.o"
echo "  CROSS_CXX    : $CROSS_CXX"
