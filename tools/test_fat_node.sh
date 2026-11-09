#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
clang -std=c11 -Wall -Wextra -Werror -Itools/fat-node-tests/include -Ikernel/fs kernel/fs/FATNode.c tools/fat-node-tests/fat_node_test.c -o build/fat_node_test.exe
./build/fat_node_test.exe
