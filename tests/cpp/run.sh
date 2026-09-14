#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/libivy-anchor-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
make -C "$repo_dir/src" static-libs
${CXX:-c++} -std=c++17 -UNDEBUG -I"$repo_dir/src" "$repo_dir/tests/cpp/anchoring_test.cpp" "$repo_dir/src/libivy.a" $(pcre2-config --libs8) -pthread -o "$tmp_dir/anchoring_test"
"$tmp_dir/anchoring_test"
