#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase7-test
port_base=$((25100 + ($$ % 250)))
port_a=${IVY_PHASE7_BUS_PORT_A:-$port_base}
port_b=${IVY_PHASE7_BUS_PORT_B:-$((port_base + 300))}
bus_a=${IVY_PHASE7_BUS_A:-127.255.255.255:$port_a}
bus_b=${IVY_PHASE7_BUS_B:-127.255.255.255:$port_b}
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase7_query_buffer_test.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase7_query_buffer_test"

"$tmp_dir/phase7_query_buffer_test" "$bus_a" "$bus_b"

echo "phase7 query buffer tests passed"
