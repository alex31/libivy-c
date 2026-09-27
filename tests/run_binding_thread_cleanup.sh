#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/ivy-binding-cleanup.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cc=${CC:-cc}
pcre_cflags=${PCRE_CFLAGS:-$(pcre2-config --cflags)}
pcre_libs=${PCRE_LIBS:-$(pcre2-config --libs8)}

# Compile in a temporary directory so sanitizer flags never contaminate the
# normal library build. LeakSanitizer must run on exit to catch lost TLS caches.
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -UNDEBUG \
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -DUSE_PCRE_REGEX -DPCRE_OPT=0 -I"$repo_dir/src" $pcre_cflags \
    "$repo_dir/tests/binding_thread_cleanup_test.c" "$repo_dir/src/ivybind.c" \
    $pcre_libs -o "$tmp_dir/binding_thread_cleanup_test"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$tmp_dir/binding_thread_cleanup_test"
