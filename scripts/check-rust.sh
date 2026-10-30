#!/bin/bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -eq 0 || "${1:-}" == --help ]]; then
    echo 'Usage: check-rust.sh PACKAGE [-- CARGO_OPTIONS...]'
    echo 'Checks a snow-crates package. Pass --no-default-features for platform-specific crates as needed.'
    [[ $# -gt 0 ]] && exit 0
    exit 2
fi
package="$1"; shift
[[ "${1:-}" != -- ]] || shift
cd "$repo_root/snow-crates"
toolchain=()
if [[ "$package" == snow-ocr-process ]]; then
    pin="$repo_root/snow-crates/crates/simd-paddle-ocr-rs/rust-toolchain.toml"
    channel="$(sed -n 's/^channel *= *"\([^"]*\)".*/\1/p' "$pin")"
    [[ "$channel" == nightly-* ]] || { echo 'Invalid OCR toolchain pin' >&2; exit 1; }
    toolchain=("+$channel")
fi
cargo "${toolchain[@]}" fmt -p "$package" -- --check
cargo "${toolchain[@]}" clippy --locked -p "$package" --all-targets "$@" -- -D warnings
