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
for compile_case in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 \
    22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 \
    46 47 48 49 50 51; do
    if "$cxx" -std=c++23 -fsyntax-only -DIVY_COMPILE_CASE="$compile_case" \
        -I"$repo_dir/src/cpp" -I"$repo_dir/src" "$repo_dir/tests/cpp/compile_test.cpp" \
        >"$tmp_dir/compile-$compile_case.log" 2>&1; then
        echo "Invalid C++ API case $compile_case unexpectedly compiled" >&2
        exit 1
    fi
done
echo "C++ API argument, anchoring and formatting checks passed"

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

"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/mainloop_test.cpp" "$archive" "$repo_dir/src/libivy.a" \
    $(pcre2-config --libs8) -pthread -o "$tmp_dir/mainloop_test"

# Check the installed header layout and pkg-config link flags in a private prefix.
stage=$tmp_dir/stage
make -C "$repo_dir/src" includes installpkgconf install-cpp DESTDIR="$stage" PREFIX=/usr
test ! -e "$stage/usr/include/Ivy/ivy_query_internal.h"
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
# Every documented API section can be opened/included directly, and repeated
# includes must still assemble exactly one complete Bus definition.
for api_header in "$stage/usr/include/Ivy/api/"*.hpp; do
    "$cxx" -std=c++23 -Wall -Wextra -Wpedantic -fsyntax-only -x c++ \
        $(pkg-config --cflags ivy-cpp) - <<EOF
#include <Ivy/api/${api_header##*/}>
#if defined(IVY_CPP_API_HEADERS)
#error "The API assembly macro must not escape the header."
#endif
static_assert(std::is_move_constructible_v<ivy::Bus>);
void header_check(ivy::Bus& bus) {
    (void)bus.bind_raw([](IvyClientPtr, auto) {}, "^HEADER (.*)$");
    (void)bus.bind_raw([](auto args) { (void)args.size(); }, "^CAPTURES (.*)$");
    (void)bus.bind_convert([](ivy::ConvertStatus, long, double, std::string_view, bool) {}, "^TYPED (.*) (.*) (.*) (.*)$");
    (void)bus.bind_convert([](IvyClientPtr, ivy::ConvertStatus, long) {}, "^TYPED_PEER (.*)$");
    (void)bus.bind_direct([](IvyClientPtr, int, std::string_view) {});
    (void)bus.bind_event([](IvyClientPtr, int) {}, ivy::pong);
    (void)bus.send("HEADER {}", 42);
    (void)bus.set_filters("HEADER");
}
#include <Ivy/ivy.hpp>
#include <Ivy/api/send.hpp>
#include <Ivy/api/lifecycle.hpp>
EOF
done
echo "Installed API sections and repeated includes compile"

for example in lifecycle callbacks inspection ivytranslater; do
    "$cxx" -std=c++23 -Wall -Wextra -Wpedantic \
        $(pkg-config --cflags ivy-cpp) "$repo_dir/examples/cpp/$example.cpp" \
        $(pkg-config --libs ivy-cpp) -o "$tmp_dir/${example}_example"
done

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
timeout 20s "$tmp_dir/mainloop_test" "127.255.255.255:$((port + 4))"
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$stage/usr/include/Ivy" $(pkg-config --cflags ivy-cpp) \
    "$repo_dir/tests/cpp/mainloop_test.cpp" $(pkg-config --libs ivy-cpp) \
    -o "$tmp_dir/mainloop_shared_test"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 20s "$tmp_dir/mainloop_shared_test" "127.255.255.255:$((port + 5))"
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$repo_dir/src/cpp" -I"$repo_dir/src" \
    "$repo_dir/tests/cpp/thread_test.cpp" "$archive" "$repo_dir/src/libivy.a" \
    $(pcre2-config --libs8) -pthread -o "$tmp_dir/thread_test"
timeout 20s "$tmp_dir/thread_test" "127.255.255.255:$((port + 6))"
cc -shared -fPIC "$repo_dir/tests/cpp/thread_failure.c" -o "$tmp_dir/thread_failure.so"
LD_PRELOAD="$tmp_dir/thread_failure.so" \
    timeout 20s "$tmp_dir/thread_test" "127.255.255.255:$((port + 7))" --thread-failure
"$cxx" -std=c++23 -O2 -g -Wall -Wextra -Wpedantic -UNDEBUG \
    -I"$stage/usr/include/Ivy" $(pkg-config --cflags ivy-cpp) \
    "$repo_dir/tests/cpp/thread_test.cpp" $(pkg-config --libs ivy-cpp) \
    -o "$tmp_dir/thread_shared_test"
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 20s "$tmp_dir/thread_shared_test" "127.255.255.255:$((port + 8))"
echo "C++23 static/shared wrapper and installed consumer checks passed"
