#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_dir=${IVY_DEB_OUTPUT_DIR:-$repo_dir/build/debian}
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ivy-deb-build.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
mkdir "$build_dir/source"

# Build in a disposable copy: dpkg's clean step must not remove local builds.
tar -C "$repo_dir" \
    --exclude='src/build' --exclude='examples/ivyqt/build' \
    --exclude='debian/tmp' --exclude='debian/.debhelper' \
    --exclude='debian/ivy-c' --exclude='debian/ivy-c-dev' \
    --exclude='debian/ivy-cpp' --exclude='debian/ivy-cpp-dev' \
    --exclude='*.o' --exclude='*.a' --exclude='*.so' --exclude='*.so.*' \
    --exclude='*.deb' --exclude='*.substvars' --exclude='*.debhelper.log' \
    --exclude='debian/files' --exclude='debian/debhelper-build-stamp' \
    -cf "$build_dir/source.tar" src tools doc examples debian README.md
tar -C "$build_dir/source" -xf "$build_dir/source.tar"

cd "$build_dir/source"
dpkg-buildpackage --build=binary --no-sign "$@"

# Only copy outputs from this successful invocation.
for artifact in "$build_dir"/*.deb "$build_dir"/*.buildinfo "$build_dir"/*.changes; do
    test ! -f "$artifact" || cp "$artifact" "$output_dir/"
done
printf '\nDebian packages written to %s\n' "$output_dir"
