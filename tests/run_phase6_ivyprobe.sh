#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase6-ivyprobe-test
port_a=${IVY_PHASE6_PROBE_BUS_PORT_A:-$((24400 + ($$ % 300)))}
port_b=${IVY_PHASE6_PROBE_BUS_PORT_B:-$((24750 + ($$ % 300)))}
bus_a=${IVY_PHASE6_PROBE_BUS_A:-127.255.255.255:$port_a}
bus_b=${IVY_PHASE6_PROBE_BUS_B:-127.255.255.255:$port_b}
fifo=$tmp_dir/ivyprobe.in
log=$tmp_dir/ivyprobe.log
probe_pid=

cleanup()
{
	status=$?
	exec 3>&- 2>/dev/null || true
	if [ -n "$probe_pid" ]; then
		kill "$probe_pid" 2>/dev/null || true
		wait "$probe_pid" 2>/dev/null || true
	fi
	rm -f "$fifo"
	exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmp_dir"
rm -f "$fifo" "$log"
mkfifo "$fifo"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase6_ivyprobe_peer.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase6_ivyprobe_peer"

IVYBUS=$bus_a "$repo_dir/tools/ivyprobe" -n phase6-probe -b "$bus_b" "^PROBE_TEST (.*)" \
	< "$fifo" > "$log" 2>&1 &
probe_pid=$!
exec 3>"$fifo"

sleep 1
"$tmp_dir/phase6_ivyprobe_peer" sender-a "$bus_a" bus_a
"$tmp_dir/phase6_ivyprobe_peer" sender-b "$bus_b" bus_b
sleep 1
printf ".quit\n" >&3
wait "$probe_pid"
probe_pid=

if ! rg -q "\\[$bus_a\\] sender-a sent  'bus_a'" "$log"; then
	echo "missing sender-a message from IVYBUS $bus_a" >&2
	cat "$log" >&2
	exit 1
fi

if ! rg -q "\\[$bus_b\\] sender-b sent  'bus_b'" "$log"; then
	echo "missing sender-b message from -b $bus_b" >&2
	cat "$log" >&2
	exit 1
fi

echo "phase6 ivyprobe multibus test passed"
