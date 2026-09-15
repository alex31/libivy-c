#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
qt_tmp=$(mktemp -d "${TMPDIR:-/tmp}/libivy-qt-test.XXXXXX")
trap 'rm -rf "$qt_tmp"' EXIT HUP INT TERM
qt_stage=$qt_tmp/install
make -C "$repo_dir/src" cpp
make -C "$repo_dir/src" includes installpkgconf install-cpp PREFIX="$qt_stage"
version=$(pkg-config --modversion "$repo_dir/src/ivy-c.pc")
major=${version%%.*}
install -m644 "$repo_dir/src/libivy.a" "$repo_dir/src/libivy.so.$version" "$qt_stage/lib/"
ln -s "libivy.so.$version" "$qt_stage/lib/libivy.so"
ln -s "libivy.so.$version" "$qt_stage/lib/libivy.so.$major"
export PKG_CONFIG_PATH=$qt_stage/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
export LD_LIBRARY_PATH=$qt_stage/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export IVY_QT_TEST_BUS=${IVY_QT_TEST_BUS:-127.255.255.255:$((32000 + ($$ % 1000)))}
cmake -S "$repo_dir/examples/ivyqt" -B "$qt_tmp/build" \
    -DCMAKE_BUILD_TYPE=Debug -DIVY_QT_BUILD_TESTS=ON -DCMAKE_CXX_COMPILER="${CXX:-c++}"
cmake --build "$qt_tmp/build" --parallel 2
ctest --test-dir "$qt_tmp/build" --output-on-failure
# The demo must use the C++ wrapper; the C API remains inside that library.
if rg -n 'IvyContext[A-Za-z]+\s*\(|Ivy(?:Init|Start|Stop|BindMsg|SendMsg)\s*\(|native_handle\(' \
    "$repo_dir/examples/ivyqt" -g '*.cpp' -g '*.hpp'; then
    echo 'Qt example still calls the C API' >&2
    exit 1
fi
echo 'Qt6 C++23 example, live traffic and asynchronous shutdown passed'
