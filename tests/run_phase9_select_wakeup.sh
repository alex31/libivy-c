#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase9-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase9_select_wakeup_test.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase9_select_wakeup_test"

port=$((23000 + ($$ % 1000)))
"$tmp_dir/phase9_select_wakeup_test" "127:$port"

echo "phase9 select wakeup test passed"
