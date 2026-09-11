#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase11-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
	-O2 -Wall -Wshadow -fPIC -march=x86-64-v3 -mtune=generic \
	-I"$repo_dir/src" -c "$repo_dir/src/ivyloop.c" \
	-o "$tmp_dir/ivyloop_phase11_testing.o"

cc -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
	-O2 -Wall -Wshadow -fPIC -march=x86-64-v3 -mtune=generic \
	-I"$repo_dir/src" -c "$repo_dir/src/ivysocket.c" \
	-o "$tmp_dir/ivysocket_phase11_testing.o"

cc -DIVY_TESTING -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase11_runtime_errors_test.c" \
	"$repo_dir/src/ivy.o" \
	"$tmp_dir/ivyloop_phase11_testing.o" \
	"$repo_dir/src/timer.o" \
	"$tmp_dir/ivysocket_phase11_testing.o" \
	"$repo_dir/src/ivybuffer.o" \
	"$repo_dir/src/ivyfifo.o" \
	"$repo_dir/src/ivybind.o" \
	"$repo_dir/src/intervalRegexp.o" \
	"$repo_dir/src/param.o" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase11_runtime_errors_test"

"$tmp_dir/phase11_runtime_errors_test"

echo "phase11 runtime error tests passed"
