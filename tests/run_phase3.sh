#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase3-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase3_threading_test.c" \
	"$repo_dir/src/ivyloop.o" \
	"$repo_dir/src/timer.o" \
	"$repo_dir/src/ivysocket.o" \
	"$repo_dir/src/ivy.o" \
	"$repo_dir/src/ivybuffer.o" \
	"$repo_dir/src/ivyfifo.o" \
	"$repo_dir/src/ivybind.o" \
	"$repo_dir/src/intervalRegexp.o" \
	"$repo_dir/src/param.o" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase3_threading_test"

"$tmp_dir/phase3_threading_test"

echo "phase3 threading tests passed"
