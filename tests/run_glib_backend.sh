#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-glib-test.XXXXXX")
trap 'rm -f "$tmp_dir/glib_backend_test" "$tmp_dir/glib_backend_shared_test" "$tmp_dir/glib_backend_test.o" "$tmp_dir/libglibivy.so.3"; rmdir "$tmp_dir"' EXIT HUP INT TERM

make -C "$repo_dir/src" static-libs shared-libs
cc ${CFLAGS:--O2 -g -Wall -Wshadow} -I"$repo_dir/src" \
  $(pkg-config --cflags glib-2.0) \
  -c "$repo_dir/tests/glib_backend_test.c" -o "$tmp_dir/glib_backend_test.o"
cc "$tmp_dir/glib_backend_test.o" "$repo_dir/src/libglibivy.a" \
  $(pkg-config --libs glib-2.0) $(pcre2-config --libs8) -pthread \
  ${LDFLAGS:-} -o "$tmp_dir/glib_backend_test"

G_DEBUG=fatal-warnings "$tmp_dir/glib_backend_test" \
  "${IVY_GLIB_TEST_BUS:-127.255.255.255:$((25000 + ($$ % 1000)))}"

# An optional argument tests an installed/packaged shared library as well.
shared_lib=${1:-$repo_dir/src/libglibivy.so.3.17}
test -f "$shared_lib"
ln -s "$shared_lib" "$tmp_dir/libglibivy.so.3"
cc "$tmp_dir/glib_backend_test.o" -L"$tmp_dir" -l:libglibivy.so.3 \
  $(pkg-config --libs glib-2.0) -pthread ${LDFLAGS:-} \
  -Wl,-rpath,"$tmp_dir" -o "$tmp_dir/glib_backend_shared_test"
G_DEBUG=fatal-warnings "$tmp_dir/glib_backend_shared_test" \
  "${IVY_GLIB_TEST_BUS:-127.255.255.255:$((25000 + ($$ % 1000)))}"
