#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase4-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
	-O2 -Wall -Wshadow -fPIC -march=x86-64-v3 -mtune=generic \
	-I"$repo_dir/src" -c "$repo_dir/src/ivy.c" -o "$tmp_dir/ivy_phase4_testing.o"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase4_wakeup_control_test.c" \
	"$tmp_dir/ivy_phase4_testing.o" \
	"$repo_dir/src/ivyloop.o" \
	"$repo_dir/src/timer.o" \
	"$repo_dir/src/ivysocket.o" \
	"$repo_dir/src/ivybuffer.o" \
	"$repo_dir/src/ivyfifo.o" \
	"$repo_dir/src/ivybind.o" \
	"$repo_dir/src/intervalRegexp.o" \
	"$repo_dir/src/param.o" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase4_wakeup_control_test"

"$tmp_dir/phase4_wakeup_control_test"

echo "phase4 wakeup/control tests passed"
