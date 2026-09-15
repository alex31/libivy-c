#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-query-snapshots.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

make -C "$repo_dir/src" static-libs shared-libs
cc -std=gnu11 -O2 -g -Wall -Wextra -UNDEBUG -I"$repo_dir/src" \
    "$repo_dir/tests/query_snapshot_test.c" "$repo_dir/src/libivy.a" \
    $(pcre2-config --libs8) -pthread -Wl,--wrap=calloc -Wl,--wrap=strdup \
    -o "$tmp_dir/query_snapshot_test"
"$tmp_dir/query_snapshot_test" "127.255.255.255:$((28000 + ($$ % 1000)))"
