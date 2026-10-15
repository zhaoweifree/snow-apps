#!/usr/bin/env bash
# Per-crate Linux cross-check for snow-crates.
#
# Why this exists
# ---------------
# `cargo check --target <linux-triple>` only needs rustc + std, not a linker,
# so it runs on the Windows dev box. That turns "Linux-only compile errors" into
# something we find here instead of on the UOS machine.
#
# Checking the *whole workspace at once* is not useful yet: a single C-backed
# dependency (`ring`, via cc-rs) aborts the run before the compiler ever reaches
# the snow-* crates. Checking one crate at a time isolates "our code is wrong"
# from "this dependency needs a C cross-compiler".
#
# Usage:
#   bash scripts/linux-cross-check.sh                    # x86_64 only
#   SNOW_CHECK_TARGETS="x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu" \
#       bash scripts/linux-cross-check.sh
#   SNOW_CHECK_FILTER=capture bash scripts/linux-cross-check.sh
#
# Exit status is 0 when every checked crate passes, 1 otherwise.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CRATES="$ROOT/snow-crates"
LOG_DIR="${SNOW_CHECK_LOG_DIR:-$ROOT/.workbuddy/qt5-port/logs}"
REPORT="${SNOW_CHECK_REPORT:-$LOG_DIR/linux-cross-check.txt}"

TARGETS="${SNOW_CHECK_TARGETS:-x86_64-unknown-linux-gnu}"
FILTER="${SNOW_CHECK_FILTER:-}"

mkdir -p "$LOG_DIR"

PY="${SNOW_PYTHON:-}"
if [ -z "$PY" ]; then
    for candidate in python python3 py; do
        if command -v "$candidate" >/dev/null 2>&1; then
            PY="$candidate"
            break
        fi
    done
fi
if [ -z "$PY" ]; then
    echo "!! no python interpreter found; set SNOW_PYTHON" >&2
    exit 1
fi

# rustup's shims live outside the default Git Bash PATH on Windows dev boxes.
export PATH="$HOME/.cargo/bin:$PATH"
command -v cargo >/dev/null 2>&1 || {
    echo "!! cargo not found on PATH" >&2
    exit 1
}

cd "$CRATES" || exit 1

# Cargo is the authority on which packages exist; do not hand-maintain a list.
# `tr -d '\r'` matters: a Windows Python writes CRLF, and a trailing carriage
# return turns every package name into an invalid cargo argument.
PACKAGES="$(
    cargo metadata --no-deps --format-version 1 2>/dev/null |
        "$PY" -c 'import json,sys; print("\n".join(sorted(p["name"] for p in json.load(sys.stdin)["packages"])))' |
        tr -d '\r' |
        sed '/^$/d'
)" || {
    echo "!! could not read package list from cargo metadata" >&2
    exit 1
}

if [ -n "$FILTER" ]; then
    PACKAGES="$(printf '%s\n' "$PACKAGES" | grep -- "$FILTER")"
fi

{
    echo "=== linux cross-check ==="
    echo "crates : $CRATES"
    echo "targets: $TARGETS"
    echo "date   : $(date '+%Y-%m-%d %H:%M:%S')"
    echo
} | tee "$REPORT"

failures=0
checked=0
for target in $TARGETS; do
    echo "--- target: $target ---" | tee -a "$REPORT"
    for crate in $PACKAGES; do
        checked=$((checked + 1))
        crate_log="$LOG_DIR/linux-check-$target-$crate.log"
        if cargo check -p "$crate" --target "$target" >"$crate_log" 2>&1; then
            printf '  PASS  %s\n' "$crate" | tee -a "$REPORT"
        else
            failures=$((failures + 1))
            # Surface the first diagnostic so the report is actionable on its own.
            first=$(
                grep -m1 -E '^error(\[[A-Z0-9]+\])?:' "$crate_log" |
                    cut -c1-160
            )
            [ -n "$first" ] || first="(no rustc error; see log, likely a C/ffmpeg dependency)"
            printf '  FAIL  %-28s %s\n' "$crate" "$first" | tee -a "$REPORT"
        fi
    done
    echo | tee -a "$REPORT"
done

{
    echo "checked=$checked failures=$failures"
    echo "per-crate logs: $LOG_DIR/linux-check-<target>-<crate>.log"
} | tee -a "$REPORT"

[ "$failures" -eq 0 ]
