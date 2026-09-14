#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/ivy-transport-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
make -C "$repo_dir/src" static-libs shared-libs libivy_omp.a
variants='normal omp'
for variant in $variants; do
    extra_libs=''
    extra_flags=''
    case "$variant" in
        omp) library=libivy_omp.a ;;
        glib) library=libglibivy.a
              extra_libs=$(pkg-config --libs glib-2.0)
              extra_flags="-DTRANSPORT_GLIB $(pkg-config --cflags glib-2.0)" ;;
        *) library=libivy.a ;;
    esac
    cc -std=c11 -O1 -g -Wall -Wextra -UNDEBUG $extra_flags -I"$repo_dir/src" \
        "$repo_dir/tests/transport_errors_test.c" \
        "$repo_dir/src/$library" $(pcre2-config --libs8) $extra_libs -pthread -fopenmp \
        -o "$tmp_dir/transport_test"
    echo "Transport variant: $variant"
    OMP_NUM_THREADS=2 timeout 30 "$tmp_dir/transport_test" "$((29000 + ($$ % 300)))"
done
