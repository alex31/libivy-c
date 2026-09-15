#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-cpp-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cxx=${CXX:-c++}

make -C "$repo_dir/src" cpp
archive=$repo_dir/src/build/cpp/libivy-cpp.a

# The positive case must compile before any rejected cases are counted as passes.
"$cxx" -std=c++23 -fsyntax-only -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/compile_test.cpp"
for compile_case in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 16 19 20; do
    if "$cxx" -std=c++23 -fsyntax-only -DIVY_COMPILE_CASE="$compile_case" \
        -I"$repo_dir/src/cpp" -I"$repo_dir/src" "$repo_dir/tests/cpp/compile_test.cpp" \
        >"$tmp_dir/compile-$compile_case.log" 2>&1; then
        echo "Invalid anchoring/format case $compile_case unexpectedly compiled" >&2
        exit 1
    fi
done
echo "Anchoring and send-format compile-time checks passed"

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/bus_test.cpp" "$archive" -pthread \
    -o "$tmp_dir/bus_test"
"$tmp_dir/bus_test"

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/anchoring_test.cpp" "$archive" "$repo_dir/src/libivy.a" \
    $(pcre2-config --libs8) -pthread -o "$tmp_dir/anchoring_test"
"$tmp_dir/anchoring_test"

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/integration_test.cpp" "$archive" \
    "$repo_dir/src/libivy.a" $(pcre2-config --libs8) -pthread \
    -o "$tmp_dir/integration_test"

# Check the installed header layout and pkg-config link flags in a private prefix.
stage=$tmp_dir/stage
make -C "$repo_dir/src" includes installpkgconf install-cpp DESTDIR="$stage" PREFIX=/usr
version=$(pkg-config --modversion "$repo_dir/src/ivy-c.pc")
major=${version%%.*}
install -m644 "$repo_dir/src/libivy.a" "$repo_dir/src/libivy.so.$version" "$stage/usr/lib/"
ln -s "libivy.so.$version" "$stage/usr/lib/libivy.so"
ln -s "libivy.so.$version" "$stage/usr/lib/libivy.so.$major"
test "$(readlink "$stage/usr/lib/libivy-cpp.so")" = "libivy-cpp.so.$version"
test "$(readlink "$stage/usr/lib/libivy-cpp.so.$major")" = "libivy-cpp.so.$version"
test -f "$stage/usr/lib/libivy-cpp.a"
test "$(readelf -d "$stage/usr/lib/libivy-cpp.so" | awk '/SONAME/ { print $NF }')" = "[libivy-cpp.so.$major]"
dependencies=$(readelf -d "$stage/usr/lib/libivy-cpp.so")
case "$dependencies" in
    *"[libivy.so.$major]"*) ;;
    *) echo "C++ shared library does not depend on the C shared library" >&2; exit 1 ;;
esac
export PKG_CONFIG_LIBDIR=$stage/usr/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$stage
unset PKG_CONFIG_PATH
"$cxx" -std=c++23 -Wall -Wextra -Wpedantic \
    $(pkg-config --cflags ivy-cpp) "$repo_dir/examples/cpp/lifecycle.cpp" \
    $(pkg-config --libs ivy-cpp) -o "$tmp_dir/lifecycle_example"

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$stage/usr/include/Ivy" $(pkg-config --cflags ivy-cpp) \
    "$repo_dir/tests/cpp/integration_test.cpp" $(pkg-config --libs ivy-cpp) \
    -o "$tmp_dir/integration_shared_test"
dependencies=$(readelf -d "$tmp_dir/integration_shared_test")
case "$dependencies" in
    *"[libivy-cpp.so.$major]"*) ;;
    *) echo "Consumer did not link to the shared wrapper" >&2; exit 1 ;;
esac

port=$((27000 + ($$ % 1000)))
"$tmp_dir/integration_test" "127.255.255.255:$port" "127.255.255.255:$((port + 1100))"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$tmp_dir/integration_shared_test" "127.255.255.255:$((port + 2))" "127.255.255.255:$((port + 1102))"
echo "C++23 static/shared wrapper and installed consumer checks passed"
