#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT_DIR/build/nxsc"
CASES_DIR="$ROOT_DIR/tests/bash-compat/cases"

pass_count=0

for nxs_file in "$CASES_DIR"/*.nxs; do
    base="${nxs_file%.nxs}"
    sh_file="${base}.sh"

    if [[ ! -f "$sh_file" ]]; then
        echo "missing paired shell file: $sh_file" >&2
        exit 1
    fi

    nxs_out="$(mktemp)"
    nxs_err="$(mktemp)"
    sh_out="$(mktemp)"
    sh_err="$(mktemp)"

    set +e
    "$BIN" --run "$nxs_file" >"$nxs_out" 2>"$nxs_err"
    nxs_rc=$?
    bash "$sh_file" >"$sh_out" 2>"$sh_err"
    sh_rc=$?
    set -e

    if [[ "$nxs_rc" -ne "$sh_rc" ]]; then
        echo "FAIL $(basename "$base"): exit code mismatch ($nxs_rc != $sh_rc)" >&2
        diff -u "$sh_out" "$nxs_out" || true
        diff -u "$sh_err" "$nxs_err" || true
        rm -f "$nxs_out" "$nxs_err" "$sh_out" "$sh_err"
        exit 1
    fi

    if ! diff -u "$sh_out" "$nxs_out" >/dev/null; then
        echo "FAIL $(basename "$base"): stdout mismatch" >&2
        diff -u "$sh_out" "$nxs_out" || true
        rm -f "$nxs_out" "$nxs_err" "$sh_out" "$sh_err"
        exit 1
    fi

    if ! diff -u "$sh_err" "$nxs_err" >/dev/null; then
        echo "FAIL $(basename "$base"): stderr mismatch" >&2
        diff -u "$sh_err" "$nxs_err" || true
        rm -f "$nxs_out" "$nxs_err" "$sh_out" "$sh_err"
        exit 1
    fi

    echo "PASS $(basename "$base")"
    pass_count=$((pass_count + 1))
    rm -f "$nxs_out" "$nxs_err" "$sh_out" "$sh_err"
done

echo "bash-compat: $pass_count case(s) passed"
