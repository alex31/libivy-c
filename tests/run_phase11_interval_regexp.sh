#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase11-interval-regexp-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	$(pcre2-config --cflags) \
	"$repo_dir/tests/phase11_interval_regexp_test.c" \
	"$repo_dir/src/intervalRegexp.o" \
	$(pcre2-config --libs8) \
	-o "$tmp_dir/phase11_interval_regexp_test"

"$tmp_dir/phase11_interval_regexp_test"

echo "phase11 interval regexp tests passed"
