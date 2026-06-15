#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-phase10-test
mkdir -p "$tmp_dir"

make -C "$repo_dir/src"
make -C "$repo_dir/tools" ivyprobe ivythroughput ivytestready ivyperf ivytranslater

cc -O2 -Wall -Wshadow -I"$repo_dir/src" \
	"$repo_dir/examples/testUnbind.c" \
	"$repo_dir/src/libivy.a" \
	$(pcre2-config --libs8) \
	-pthread \
	-o "$tmp_dir/testUnbind"

legacy_pattern='\\b(IvyInit|IvyStart|IvyStop|IvyMainLoop|IvyBindMsg|IvyChangeMsg|IvyUnbindMsg|IvySendMsg|IvyGetApplicationName|TimerRepeatAfter)\\b'
if command -v rg >/dev/null 2>&1; then
	if rg -n "$legacy_pattern" \
		"$repo_dir/tools/ivythroughput.cpp" \
		"$repo_dir/tools/ivyperf.c" \
		"$repo_dir/tools/ivytestready.c" \
		"$repo_dir/tools/ivytranslater.c" \
		"$repo_dir/examples/testUnbind.c"; then
		echo "legacy Ivy API found in phase10 tools" >&2
		exit 1
	fi
fi

port=$((24000 + ($$ % 1000)))
(
	cd "$repo_dir/tools"
	./ivythroughput -V -t tp -d 3 -n 1 -R 1 -M 1 -b "127:$port"
)

echo "phase10 tool smoke tests passed"
