#!/bin/bash
# Build the third-party libraries a Debian/Ubuntu image does not ship as CMake
# config packages.
#
# §9.2 decided the Linux side uses distribution packages instead of vcpkg. That
# decision survives everywhere apt has the library; it fails for the few this
# project pins *beyond* what a distribution carries:
#
#   * zlib -- here it **is** zlib-ng (`snow_image/src/compression_backend.cpp`
#     does not compile against classic zlib);
#   * libjpeg-turbo -- must be >= 3.1 *with* a CMake config (24.04 ships 2.1.5
#     and ships no config at all);
#   * lunasvg -- the SVG codec links `lunasvg::lunasvg`; 24.04 has no package
#     for it whatsoever.
#
# Windows/macOS get all three through `cmake/vcpkg-overlay-ports/` or the
# manifest baseline.
#
# Importing vcpkg on Linux to solve that is the option this script exists to
# avoid: it would build ~15 libraries on both architectures and the toolchain
# hangs on the Windows workstation (§9.6.8). Instead the same source archives
# are built with the same options the overlay ports pass.
#
# Version pins are not copied here for `zlib`: they are read out of the port's
# `vcpkg.json` (version) and `portfile.cmake` (SHA512), so bumping the port
# bumps Linux too, and the checksum proves the archive is the same one vcpkg
# fetches. Libraries with no overlay port carry their own pin below.
#
# To add one: pin version + sha512, write a `build_<name>` that ends with the
# assertion that proves the *consumer* will find it (its CMake config file, its
# defining header), then add the name to the loop at the bottom.
#
# Usage:
#   bash scripts/build-linux-deps.sh                       # everything, default prefix
#   bash scripts/build-linux-deps.sh --prefix /opt/deps     # explicit prefix
#   bash scripts/build-linux-deps.sh --only zlib            # one library
#
# After a successful run:
#   export CMAKE_PREFIX_PATH="<prefix>${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
# The script prints the line and also writes it to `<prefix>/snow-linux-deps.env`.
#
# Offline / intranet builds: put the tarballs in `<prefix>/src` (or point
# `SNOW_LINUX_DEPS_SRC` at a directory holding them) and nothing is downloaded.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="${HOME}/.local/snow-linux-deps"
only=""
clean=0

usage() {
    # Print the comment block that starts at "Usage:" -- a fixed line range
    # would silently drift every time the header above it grows.
    sed -n '/^# Usage:/,/^# The script prints/p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    echo "error: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="${2:?--prefix needs a directory}"; shift 2 ;;
        --only) only="${2:?--only needs a name}"; shift 2 ;;
        --clean) clean=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

# The recipe talks to `nproc`, `sha512sum` and installs .so files; a Windows
# shell would fail halfway through with a confusing error instead of this one.
[[ "$(uname -s)" == Linux ]] || die "this script builds Linux libraries; run it on Linux (or in the CI job)"

jobs="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
# The libraries below are built with whatever cmake is on PATH. That is
# deliberate: they are old projects whose `cmake_minimum_required` predates
# 3.5, and CMake 4 refuses those outright. The repository's own configure is
# the step that needs 4.2 (§9.6.8), not this one.
cmake_command="${CMAKE:-cmake}"
"$cmake_command" --version >/dev/null 2>&1 || die "cmake not found; set CMAKE"
src_dir="${SNOW_LINUX_DEPS_SRC:-${prefix}/src}"
build_root="${prefix}/build"
stamp_dir="${prefix}/.stamps"
log_dir="${prefix}/logs"

mkdir -p "$src_dir" "$build_root" "$stamp_dir" "$log_dir"
prefix="$(cd "$prefix" && pwd)"

if [[ $clean -eq 1 ]]; then
    echo "== removing ${build_root} and ${stamp_dir}"
    rm -rf "$build_root" "$stamp_dir"
    mkdir -p "$build_root" "$stamp_dir"
fi

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
else
    echo "note: ninja not found, using the default Makefile generator (slower)" >&2
fi

selected() {
    [[ -z "$only" ]] && return 0
    case ",${only}," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

stamped() {
    [[ -f "${stamp_dir}/$1-$2.ok" ]]
}

stamp() {
    date -u +%Y-%m-%dT%H:%M:%SZ > "${stamp_dir}/$1-$2.ok"
}

# `--retry` on curl covers the flaky-proxy case this repository's workstation
# hits constantly (502 from the sandbox proxy, intermittently); the unpacked
# archive is what matters, not how many attempts it took.
fetch() { # url archive
    local url="$1" archive="$2"
    if [[ -s "$archive" ]]; then
        return 0
    fi
    echo "  fetching $(basename "$archive")"
    mkdir -p "$(dirname "$archive")"
    curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors -m 900 \
        -o "$archive.part" "$url"
    mv "$archive.part" "$archive"
}

verify() { # archive expected-sha512
    local archive="$1" expected="$2" actual
    actual="$(sha512sum "$archive" | cut -d' ' -f1)"
    if [[ "$actual" != "$expected" ]]; then
        die "checksum mismatch for $(basename "$archive"): got $actual"
    fi
}

unpack() { # archive work-dir
    local archive="$1" work="$2"
    rm -rf "$work"
    mkdir -p "$work"
    tar -xzf "$archive" -C "$work" --strip-components=1
}

configure_build_install() { # source build-dir extra cmake args...
    local source="$1" build="$2"
    shift 2
    "$cmake_command" -S "$source" -B "$build" ${generator[@]+"${generator[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        "$@"
    "$cmake_command" --build "$build" --parallel "$jobs"
    "$cmake_command" --install "$build"
}

# --- zlib: zlib-ng in compatibility mode -----------------------------------
# Options are the ones `cmake/vcpkg-overlay-ports/zlib/portfile.cmake` passes,
# including WITH_NATIVE_INSTRUCTIONS=OFF: the port disables `-march=native` so
# that a binary built on a CI runner still runs on a customer's UOS machine.
build_zlib() {
    local port_dir="${repo_root}/cmake/vcpkg-overlay-ports/zlib"
    local version sha url archive work build
    version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' \
        "${port_dir}/vcpkg.json")"
    sha="$(sed -n 's/^[[:space:]]*SHA512[[:space:]]\{1,\}\([0-9a-f]\{128\}\).*/\1/p' \
        "${port_dir}/portfile.cmake" | head -n1)"
    [[ -n "$sha" ]] || die "no SHA512 found in ${port_dir}/portfile.cmake"

    if stamped zlib "$version"; then
        echo "== zlib ${version}: up to date"
        return 0
    fi
    echo "== zlib ${version} (zlib-ng compatibility mode)"

    url="https://github.com/zlib-ng/zlib-ng/archive/refs/tags/${version}.tar.gz"
    archive="${src_dir}/zlib-ng-${version}.tar.gz"
    work="${build_root}/zlib-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$sha"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DZLIB_ENABLE_TESTS=OFF \
        -DZLIB_COMPAT=ON \
        -DWITH_GZFILEOP=ON \
        -DWITH_NEW_STRATEGIES=ON \
        -DWITH_NATIVE_INSTRUCTIONS=OFF \
        -DWITH_OPTIM=ON

    # The product requirement, not a proxy for it: `compression_backend.cpp`
    # refuses to compile without ZLIBNG_VERSION, and `core_tests.cpp` asserts
    # the runtime version string names zlib-ng.
    grep -q 'ZLIBNG_VERSION' "${prefix}/include/zlib.h" \
        || die "installed zlib.h has no ZLIBNG_VERSION -- the build did not use zlib-ng compatibility mode"
    stamp zlib "$version"
}

# --- libjpeg-turbo ----------------------------------------------------------
# No overlay port of our own, so the pin lives here. 3.1 is what
# `snow_image/CMakeLists.txt` asks for (`find_package(libjpeg-turbo 3.1 CONFIG
# REQUIRED)`); Ubuntu 24.04 has 2.1.5 and ships no CMake config at all. The
# shared library is enough -- every consumer links the target, which carries the
# path with it.
libjpeg_turbo_version="3.1.0"
libjpeg_turbo_sha512="5712d318e222f1ffcd2f748b0f2c32b3859253a4ed4e13ae134f4445e0ca06efc258c7653b6924b39815ae078f6a9177e098c89684d2c886161a0a4118122e8d"

build_libjpeg_turbo() {
    local version="$libjpeg_turbo_version"
    local url archive work build config

    if stamped libjpeg-turbo "$version"; then
        echo "== libjpeg-turbo ${version}: up to date"
        return 0
    fi
    echo "== libjpeg-turbo ${version}"

    url="https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/${version}.tar.gz"
    archive="${src_dir}/libjpeg-turbo-${version}.tar.gz"
    work="${build_root}/libjpeg-turbo-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$libjpeg_turbo_sha512"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DENABLE_SHARED=ON \
        -DENABLE_STATIC=OFF \
        -DWITH_TURBOJPEG=ON \
        -DWITH_SIMD=ON \
        -DWITH_TOOLS=OFF \
        -DWITH_TESTS=OFF

    # `find_package(libjpeg-turbo 3.1 CONFIG REQUIRED)` reads this file; without
    # it the library might as well not be installed.
    config="$(find "${prefix}" -name 'libjpeg-turboConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "libjpeg-turbo installed no CMake config package"
    stamp libjpeg-turbo "$version"
}

# --- lunasvg ----------------------------------------------------------------
# `snow_image`'s SVG codec links `lunasvg::lunasvg`. Ubuntu 24.04 has no such
# package at all (unlike libwebp/libheif/libjxl, which are only too old or
# lack a config file), so this is a build-from-source, not a version bump.
lunasvg_version="3.5.0"
lunasvg_sha512="62cf1433f4d158008ab07c9b6a83dca2322e1adf97a7f30a2021be5610af7b28e2ed54c75292fb382a3dcc6205f0e7b8d815b8a59aa11843a059883203d191c1"

build_lunasvg() {
    local version="$lunasvg_version"
    local url archive work build config

    if stamped lunasvg "$version"; then
        echo "== lunasvg ${version}: up to date"
        return 0
    fi
    echo "== lunasvg ${version}"

    url="https://github.com/sammycage/lunasvg/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/lunasvg-${version}.tar.gz"
    work="${build_root}/lunasvg-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$lunasvg_sha512"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DBUILD_SHARED_LIBS=ON \
        -DLUNASVG_BUILD_EXAMPLES=OFF

    # The target `snow_image` links is `lunasvg::lunasvg`; the config file is
    # what defines it, so an install without one is an install that does nothing.
    config="$(find "${prefix}" \( -name 'lunasvg-config.cmake' -o -name 'lunasvgConfig.cmake' \) -print -quit)"
    [[ -n "$config" ]] || die "lunasvg installed no CMake config package"
    stamp lunasvg "$version"
}

for library in zlib libjpeg-turbo lunasvg; do
    if ! selected "$library"; then
        continue
    fi
    log="${log_dir}/${library}.log"
    case "$library" in
        zlib) build_zlib >"$log" 2>&1 || { cat "$log"; die "zlib build failed (log: ${log})"; } ;;
        libjpeg-turbo) build_libjpeg_turbo >"$log" 2>&1 || { cat "$log"; die "libjpeg-turbo build failed (log: ${log})"; } ;;
        lunasvg) build_lunasvg >"$log" 2>&1 || { cat "$log"; die "lunasvg build failed (log: ${log})"; } ;;
    esac
    echo "   $(tail -n 1 "$log")"
done

env_file="${prefix}/snow-linux-deps.env"
{
    echo "# Generated by scripts/build-linux-deps.sh -- source this before configuring."
    echo "export CMAKE_PREFIX_PATH=\"${prefix}\${CMAKE_PREFIX_PATH:+:\${CMAKE_PREFIX_PATH}}\""
} >"$env_file"

echo
echo "installed into ${prefix}"
echo "  source ${env_file}"
echo "  # or: cmake --preset snow-shot-linux-x64-release -DCMAKE_PREFIX_PATH=${prefix}"
