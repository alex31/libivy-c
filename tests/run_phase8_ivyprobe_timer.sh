#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase8-ivyprobe-timer-test
port_a=${IVY_PHASE8_TIMER_BUS_PORT_A:-$((25700 + ($$ % 250)))}
port_b=${IVY_PHASE8_TIMER_BUS_PORT_B:-$((26000 + ($$ % 250)))}
bus_a=${IVY_PHASE8_TIMER_BUS_A:-127.255.255.255:$port_a}
bus_b=${IVY_PHASE8_TIMER_BUS_B:-127.255.255.255:$port_b}
fifo=$tmp_dir/ivyprobe.in
log=$tmp_dir/ivyprobe.log
peer_a_log=$tmp_dir/peer-a.log
peer_b_log=$tmp_dir/peer-b.log
probe_pid=
peer_a_pid=
peer_b_pid=

cleanup()
{
	status=$?
	exec 3>&- 2>/dev/null || true
	for pid in "$probe_pid" "$peer_a_pid" "$peer_b_pid"; do
		if [ -n "$pid" ]; then
			kill "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	done
	rm -f "$fifo"
	exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmp_dir"
rm -f "$fifo" "$log" "$peer_a_log" "$peer_b_log"
mkfifo "$fifo"

make -C "$repo_dir/src"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase8_ivyprobe_timer_peer.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/phase8_ivyprobe_timer_peer"

"$tmp_dir/phase8_ivyprobe_timer_peer" phase8-timer-a "$bus_a" \
	> "$peer_a_log" 2>&1 &
peer_a_pid=$!
"$tmp_dir/phase8_ivyprobe_timer_peer" phase8-timer-b "$bus_b" \
	> "$peer_b_log" 2>&1 &
peer_b_pid=$!

IVYBUS=$bus_a "$repo_dir/tools/ivyprobe" -n phase8-probe \
	-b "$bus_b" -w 2 -t < "$fifo" > "$log" 2>&1 &
probe_pid=$!
exec 3>"$fifo"

peer_status=0
wait "$peer_a_pid" || peer_status=$?
peer_a_pid=
wait "$peer_b_pid" || peer_status=$?
peer_b_pid=

printf ".quit\n" >&3
wait "$probe_pid"
probe_pid=

if [ "$peer_status" -ne 0 ]; then
	echo "timer peer failed" >&2
	cat "$peer_a_log" >&2
	cat "$peer_b_log" >&2
	cat "$log" >&2
	exit "$peer_status"
fi

if ! rg -q "Timer callback: 1" "$log"; then
	echo "missing timer callback 1 in ivyprobe log" >&2
	cat "$log" >&2
	exit 1
fi

if ! rg -q "Timer callback: 5" "$log"; then
	echo "missing timer callback 5 in ivyprobe log" >&2
	cat "$log" >&2
	exit 1
fi

echo "phase8 ivyprobe timer multibus test passed"
