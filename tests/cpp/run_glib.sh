#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-cpp-glib-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cxx=${CXX:-c++}
glib_cflags=$(pkg-config --cflags glib-2.0)
glib_libs=$(pkg-config --libs glib-2.0)
system_pc_path=$(pkg-config --variable=pc_path pkg-config)
make -C "$repo_dir/src" cpp-glib

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" $glib_cflags \
    "$repo_dir/tests/cpp/glib_test.cpp" \
    "$repo_dir/src/build/cpp-glib/libivy-cpp-glib.a" "$repo_dir/src/libglibivy.a" \
    $glib_libs $(pcre2-config --libs8) -pthread -o "$tmp_dir/glib_test"
port=$((30000 + ($$ % 1000)))
G_DEBUG=fatal-warnings timeout 20s "$tmp_dir/glib_test" "127.255.255.255:$port"

stage=$tmp_dir/stage
make -C "$repo_dir/src" includes installpkgconf install-cpp-glib DESTDIR="$stage" PREFIX=/usr
version=$(pkg-config --modversion "$repo_dir/src/ivy-glib.pc")
major=${version%%.*}
install -m644 "$repo_dir/src/libglibivy.a" "$repo_dir/src/libglibivy.so.$version" "$stage/usr/lib/"
ln -s "libglibivy.so.$version" "$stage/usr/lib/libglibivy.so"
ln -s "libglibivy.so.$version" "$stage/usr/lib/libglibivy.so.$major"
test "$(readlink "$stage/usr/lib/libivy-cpp-glib.so")" = "libivy-cpp-glib.so.$version"
test "$(readlink "$stage/usr/lib/libivy-cpp-glib.so.$major")" = "libivy-cpp-glib.so.$version"
test "$(readelf -d "$stage/usr/lib/libivy-cpp-glib.so" | awk '/SONAME/ { print $NF }')" = "[libivy-cpp-glib.so.$major]"
dependencies=$(readelf -d "$stage/usr/lib/libivy-cpp-glib.so")
case "$dependencies" in
    *"[libivy.so.$major]"*) echo 'GLib wrapper accidentally links select backend' >&2; exit 1 ;;
esac
case "$dependencies" in
    *"[libglibivy.so.$major]"*) ;;
    *) echo 'GLib wrapper missing its C backend' >&2; exit 1 ;;
esac
export PKG_CONFIG_LIBDIR=$stage/usr/lib/pkgconfig:$system_pc_path
export PKG_CONFIG_SYSROOT_DIR=$stage
unset PKG_CONFIG_PATH
# GLib itself is a system dependency, not copied into the private Ivy install.
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$stage/usr/include/Ivy" $glib_cflags $(pkg-config --cflags ivy-cpp-glib) \
    "$repo_dir/tests/cpp/glib_test.cpp" $(pkg-config --libs ivy-cpp-glib) \
    -o "$tmp_dir/glib_shared_test"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    G_DEBUG=fatal-warnings timeout 20s "$tmp_dir/glib_shared_test" "127.255.255.255:$((port + 1))"
"$cxx" -std=c++23 -Wall -Wextra -Wpedantic $glib_cflags \
    $(pkg-config --cflags ivy-cpp-glib) "$repo_dir/examples/cpp/glib.cpp" \
    $(pkg-config --libs ivy-cpp-glib) -o "$tmp_dir/glib_example"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    G_DEBUG=fatal-warnings timeout 10s "$tmp_dir/glib_example" "127.255.255.255:$((port + 2))"
dependencies=$(LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$tmp_dir/glib_shared_test")
if printf '%s\n' "$dependencies" | awk '$1 ~ /^libivy(-cpp)?[.]so[.]/ { found=1 } END { exit !found }'; then
    echo 'Consumer loads both backends' >&2
    exit 1
fi
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG -DIVY_THREAD_GLIB \
    -I"$stage/usr/include/Ivy" $glib_cflags $(pkg-config --cflags ivy-cpp-glib) \
    "$repo_dir/tests/cpp/thread_test.cpp" $(pkg-config --libs ivy-cpp-glib) \
    -o "$tmp_dir/thread_shared_test"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    G_DEBUG=fatal-warnings timeout 20s "$tmp_dir/thread_shared_test" "127.255.255.255:$((port + 3))"
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG -DIVY_THREAD_GLIB \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" $glib_cflags \
    "$repo_dir/tests/cpp/thread_test.cpp" \
    "$repo_dir/src/build/cpp-glib/libivy-cpp-glib.a" "$repo_dir/src/libglibivy.a" \
    $glib_libs $(pcre2-config --libs8) -pthread -o "$tmp_dir/thread_test"
G_DEBUG=fatal-warnings timeout 20s "$tmp_dir/thread_test" "127.255.255.255:$((port + 4))"
echo "C++ GLib static/shared, installed consumer and host-loop example passed"
