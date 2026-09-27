#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/ivy-legacy-cleanup.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
make -C "$repo_dir/src" static-libs
variants=ivy
if pkg-config --exists 'glib-2.0 >= 2.36'; then variants="$variants glibivy"; fi
for variant in $variants; do
    extra_libs=''
    if [ "$variant" = glibivy ]; then extra_libs=$(pkg-config --libs glib-2.0); fi
    ${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -UNDEBUG -I"$repo_dir/src" \
        "$repo_dir/tests/legacy_cleanup_test.c" "$repo_dir/src/lib$variant.a" \
        $(pcre2-config --libs8) $extra_libs -pthread ${LDFLAGS:-} \
        -o "$tmp_dir/test"
    G_DEBUG=fatal-warnings "$tmp_dir/test" \
        "${IVY_LEGACY_CLEANUP_BUS:-127.255.255.255:$((27000 + ($$ % 1000)))}"
done
