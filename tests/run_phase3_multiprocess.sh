#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
port=${IVY_PHASE3_BUS_PORT:-$((23100 + ($$ % 400)))}
bus=${IVY_PHASE3_BUS:-127.255.255.255:$port}

make -C "$repo_dir/src"
make -C "$repo_dir/tools" ivythroughput

cd "$repo_dir/tools"
./ivythroughput \
	-V \
	-b "$bus" \
	-t tp \
	-n 3 \
	-R 0 \
	-M 3 \
	-m "$repo_dir/tests/phase3_verify_messages.ivy" \
	-d 5

echo "phase3 multiprocess delivery test passed"
