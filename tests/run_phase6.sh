#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase6-test
port_a=${IVY_PHASE6_BUS_PORT_A:-$((23600 + ($$ % 300)))}
port_b=${IVY_PHASE6_BUS_PORT_B:-$((23950 + ($$ % 300)))}
bus_a=${IVY_PHASE6_BUS_A:-127.255.255.255:$port_a}
bus_b=${IVY_PHASE6_BUS_B:-127.255.255.255:$port_b}
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
	-O2 -Wall -Wshadow -fPIC -march=x86-64-v3 -mtune=generic \
	-I"$repo_dir/src" -c "$repo_dir/src/ivy.c" -o "$tmp_dir/ivy_phase6_testing.o"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase6_context_loop_test.c" \
	"$tmp_dir/ivy_phase6_testing.o" \
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
	-o "$tmp_dir/phase6_context_loop_test"

"$tmp_dir/phase6_context_loop_test" "$bus_a" "$bus_b"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase65_public_multibus_api_test.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase65_public_multibus_api_test"

"$tmp_dir/phase65_public_multibus_api_test" "$bus_a" "$bus_b"

echo "phase6/6.5 contextual loop and public multibus API tests passed"
