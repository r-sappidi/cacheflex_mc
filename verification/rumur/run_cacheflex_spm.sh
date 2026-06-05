#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

./gen_table_model.py
rumur --deadlock-detection off --output build/cacheflex_spm_tables_complete.c \
  cacheflex_spm_tables_complete.m
cc -std=c11 -O2 -pthread -mcx16 build/cacheflex_spm_tables_complete.c \
  -o build/cacheflex_spm_tables_complete -latomic
./build/cacheflex_spm_tables_complete --threads 1

rumur --deadlock-detection off --output build/cacheflex_spm_2core.c \
  cacheflex_spm_2core.m
cc -std=c11 -O2 -pthread -mcx16 build/cacheflex_spm_2core.c \
  -o build/cacheflex_spm_2core -latomic
./build/cacheflex_spm_2core --threads 1
