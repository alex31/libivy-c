#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-context-filters.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

make -C "$repo_dir/src"
cc -std=gnu11 -DIVY_TESTING -DUSE_PCRE_REGEX -DPCRE_OPT=0 \
    -O2 -g -Wall -Wshadow -fPIC -I"$repo_dir/src" \
    -c "$repo_dir/src/ivy.c" -o "$tmp_dir/ivy_testing.o"
cc -std=gnu11 -O2 -g -Wall -Wextra -UNDEBUG -I"$repo_dir/src" \
    "$repo_dir/tests/context_filters_test.c" "$tmp_dir/ivy_testing.o" \
    "$repo_dir/src/libivy.a" $(pcre2-config --libs8) -pthread \
    -Wl,--wrap=malloc -Wl,--wrap=strdup -o "$tmp_dir/context_filters_test"
"$tmp_dir/context_filters_test"
