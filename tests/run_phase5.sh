#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase5-test
port=${IVY_PHASE5_BUS_PORT:-$((23200 + ($$ % 400)))}
bus=${IVY_PHASE5_BUS:-127.255.255.255:$port}
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase5_callback_safety_test.c" \
	-L"$repo_dir/src" -livy \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase5_callback_safety_test"

"$tmp_dir/phase5_callback_safety_test" "$bus"

echo "phase5 callback safety tests passed"
