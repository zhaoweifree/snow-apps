#!/bin/bash
# Fetch the AppImage packaging tool and the type-2 runtime it embeds.
#
# Why a script and not a `curl` line in the workflow: the pins have to live in
# exactly one place, and a truncated or replaced download has to fail loudly
# instead of producing an AppImage nobody can run.
#
# Which upstream to use is not a matter of taste:
#
#   * `AppImage/AppImageKit` is the historical home. Its releases are marked
#     obsolete (tag 13 ships every asset as `obsolete-*`), and its appimagetool
#     needs libfuse2, which current distributions no longer ship.
#   * `AppImage/appimagetool` is the maintained one, but it **does not bundle
#     the type-2 runtime** -- it downloads it live at packaging time unless a
#     runtime file is supplied. A packaging step that reaches the network on
#     its own is neither hermetic nor reproducible, so the runtime is fetched
#     here as well and handed over explicitly.
#
# Both repositories publish their assets under a rolling `continuous` tag where
# the file is *replaced in place*. A checksum pinned to that tag expires
# without anybody editing this file, so the pins below point at versioned
# releases instead. The digests are the ones the releases API publishes, and
# they were verified against the files themselves.
#
# Output contract: stdout carries only `KEY=VALUE` lines meant for
# `$GITHUB_ENV`; every progress line goes to stderr. A log line on stdout
# would become a malformed environment variable.
set -euo pipefail

APPIMAGETOOL_TAG="1.9.1"
RUNTIME_TAG="20251108"
APPIMAGETOOL_BASE_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_TAG}"
RUNTIME_BASE_URL="https://github.com/AppImage/type2-runtime/releases/download/${RUNTIME_TAG}"

# sha256 of the four assets this project uses. Read off the releases API
# digests, then confirmed against the downloaded bytes.
APPIMAGETOOL_SHA256_x86_64="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
APPIMAGETOOL_SHA256_aarch64="f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158"
RUNTIME_SHA256_x86_64="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
RUNTIME_SHA256_aarch64="00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444"

# ELF e_machine values. The type-2 runtime is a plain ELF, so this is the
# architecture of the file itself, not of something inside it.
MACHINE_x86_64="0x3e"
MACHINE_aarch64="0xb7"

usage() {
    cat >&2 <<'USAGE'
Usage: fetch-appimagetool.sh --arch <x86_64|aarch64> --dest <directory>

Downloads the pinned appimagetool and type-2 runtime for <arch> into
<directory>, verifies both against their sha256, and prints two environment
assignments on stdout:

    SNOW_APPS_APPIMAGETOOL=<path to the appimagetool AppImage>
    SNOW_APPS_APPIMAGETOOL_RUNTIME=<path to the type-2 runtime>
USAGE
}

die() {
    # Written to stderr, which is the only channel this script is allowed to
    # narrate on: stdout is parsed as environment assignments.
    echo "error: $*" >&2
    exit 1
}

arch=""
dest=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) arch="${2:-}"; shift 2 || true ;;
        --dest) dest="${2:-}"; shift 2 || true ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

[[ -n "$arch" ]] || die "--arch is required"
[[ -n "$dest" ]] || die "--dest is required"

case "$arch" in
    x86_64) machine="$MACHINE_x86_64" ;;
    aarch64) machine="$MACHINE_aarch64" ;;
    *) die "unsupported architecture '$arch' (appimagetool ships x86_64 and aarch64)" ;;
esac

tool_sha_var="APPIMAGETOOL_SHA256_${arch}"
runtime_sha_var="RUNTIME_SHA256_${arch}"
tool_sha="${!tool_sha_var:-}"
runtime_sha="${!runtime_sha_var:-}"
[[ -n "$tool_sha" ]] || die "no appimagetool checksum pinned for '$arch'"
[[ -n "$runtime_sha" ]] || die "no runtime checksum pinned for '$arch'"

mkdir -p "$dest"

# Downloads to a sidecar file and renames only after the checksum matches, so
# an interrupted transfer can never be mistaken for a finished one.
fetch() { # url destination expected_sha256
    local url="$1" target="$2" want="$3" got
    echo "fetching $url" >&2
    if ! curl --fail --location --silent --show-error \
            --retry 5 --retry-delay 3 --retry-connrefused \
            --connect-timeout 20 --max-time 900 \
            --output "${target}.part" "$url"; then
        rm -f "${target}.part"
        die "download failed: $url"
    fi
    got="$(sha256sum "${target}.part" | cut -d' ' -f1)"
    if [[ "$got" != "$want" ]]; then
        rm -f "${target}.part"
        die "checksum mismatch for $url: got ${got}, want ${want}"
    fi
    mv "${target}.part" "$target"
    echo "verified $target (sha256 ${got})" >&2
}

# Checks the ELF header: the machine must be the one asked for, and an
# AppImage must carry the type-2 magic at offset 8. The runtime is statically
# linked, so it has no dynamic loader and no GLIBC symbol versions at all --
# which is why it runs on a UOS 20 (glibc 2.28) machine unchanged.
check_elf() { # path expected_machine expect_appimage_magic
    local path="$1" want_machine="$2" want_magic="$3"
    python3 - "$path" "$want_machine" "$want_magic" <<'PY'
import sys

path, want_machine, want_magic = sys.argv[1], int(sys.argv[2], 16), sys.argv[3] == "1"
with open(path, "rb") as handle:
    head = handle.read(20)
if head[:4] != b"\x7fELF":
    sys.exit("not an ELF file: %s" % path)
machine = int.from_bytes(head[18:20], "little")
if machine != want_machine:
    sys.exit("architecture mismatch: %s has e_machine 0x%x, expected 0x%x"
             % (path, machine, want_machine))
if want_magic and head[8:11] != b"AI\x02":
    sys.exit("not a type-2 AppImage: %s (bytes 8..10 are %r)" % (path, head[8:11]))
PY
}

tool_path="${dest}/appimagetool-${arch}.AppImage"
runtime_path="${dest}/runtime-${arch}"

fetch "${APPIMAGETOOL_BASE_URL}/appimagetool-${arch}.AppImage" "$tool_path" "$tool_sha"
fetch "${RUNTIME_BASE_URL}/runtime-${arch}" "$runtime_path" "$runtime_sha"

check_elf "$tool_path" "$machine" 1
check_elf "$runtime_path" "$machine" 0
chmod +x "$tool_path" "$runtime_path"

# The tool is itself an AppImage, and a CI runner has no FUSE. The runtime
# honours APPIMAGE_EXTRACT_AND_RUN=1 by unpacking itself and running from the
# unpacked copy, which is what `snow_shot_add_appimage_target()` already sets
# for the appimagetool invocation.
echo "appimagetool ${tool_path} (${arch})" >&2
echo "runtime     ${runtime_path} (${arch})" >&2

echo "SNOW_APPS_APPIMAGETOOL=${tool_path}"
echo "SNOW_APPS_APPIMAGETOOL_RUNTIME=${runtime_path}"
