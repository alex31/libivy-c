#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=${TMPDIR:-/tmp}/libivy-c-audit-test
mkdir -p "$tmp_dir"

cc -O2 -Wall -Wextra -I"$repo_dir/src" \
	"$repo_dir/tests/audit_fifo_test.c" \
	"$repo_dir/src/ivyfifo.c" \
	-o "$tmp_dir/audit_fifo_test"

"$tmp_dir/audit_fifo_test"

echo "audit hardening tests passed"
