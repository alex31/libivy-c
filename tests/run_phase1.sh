#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase1-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"

cc -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
	-O2 -Wall -Wshadow -fPIC -march=x86-64-v3 -mtune=generic \
	-I"$repo_dir/src" -c "$repo_dir/src/ivy.c" -o "$tmp_dir/ivy_testing.o"

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/tests/phase1_context_test.c" \
	"$tmp_dir/ivy_testing.o" \
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
	-o "$tmp_dir/phase1_context_test"

"$tmp_dir/phase1_context_test"

if ! nm -a "$tmp_dir/ivy_testing.o" | awk '$2 ~ /^[bBdD]$/ {print $3}' | grep -qx default_ctx; then
	echo "default_ctx was not found as the remaining compatibility global" >&2
	exit 10
fi

legacy_globals='debug_filter debug_binary_msg ipv6 server ApplicationPort SupervisionPort broadcast ApplicationName ApplicationID direct_callback direct_user_data application_callback application_user_data application_bind_callback application_bind_data application_die_callback application_die_user_data application_pong_callback msg_recv allClients messSndByRegexp ready_message ompDictCache'

for symbol in $legacy_globals; do
	if nm -a "$tmp_dir/ivy_testing.o" | awk '$2 ~ /^[bBdD]$/ {print $3}' | grep -qx "$symbol"; then
		echo "legacy global still present: $symbol" >&2
		exit 11
	fi
done

echo "phase1 context tests passed"
