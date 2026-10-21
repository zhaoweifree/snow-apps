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
#     for it whatsoever;
#   * minizip-ng -- `snow_shot` links `MINIZIP::minizip-ng`; Ubuntu's `minizip`
#     is zlib's project, not this one;
#   * onnxruntime -- the OCR process needs it, and the port document already
#     says Linux takes Microsoft's prebuilt CPU build rather than building it;
#   * Imath + OpenEXR -- the EXR codec is written against the 3.4 API, 24.04
#     ships 3.1.5 and UOS 20 ships 2.5 (neither compiles);
#   * libwebp -- the animation codec calls `WebPAnimEncoderSetChunk()`, absent
#     from 24.04's 1.3.2 and from UOS 20's 0.6;
#   * highway + libjxl + libheif -- the JXL/HEIF codecs are written against
#     0.11 / 1.21; 24.04 carries 0.7 / 1.17, where the first does not compile
#     and the second lacks `<libheif/heif_sequences.h>`.
#
# Windows/macOS get all of them through `cmake/vcpkg-overlay-ports/` or the
# manifest baseline.
#
# Importing vcpkg on Linux to solve that is the option this script exists to
# avoid: it would build ~15 libraries on both architectures and the toolchain
# hangs on the Windows workstation (§9.6.8). Instead the same source archives
# are built with the same options the overlay ports pass -- and onnxruntime,
# which nobody builds on a laptop, is Microsoft's own prebuilt CPU package.
#
# Version pins are not copied here for `zlib` and `minizip-ng`: they are read
# out of the port's `vcpkg.json` (version) and `portfile.cmake` (SHA512), so
# bumping the port bumps Linux too, and the checksum proves the archive is the
# same one vcpkg fetches. Libraries with no overlay port carry their own pin.
# `imath` / `openexr` / `libwebp` do have ports, but they are vcpkg *builtin*
# ports living in the `.tools/vcpkg` clone, which CI does not have -- so they
# carry a copy of the pin here and `scripts/check-linux-deps-pins.py` compares
# that copy against the clone on a workstation and fails when they drift.
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

# --- minizip-ng -------------------------------------------------------------
# `snow_shot` links `MINIZIP::minizip-ng` (zip handling for the update package).
# Ubuntu ships zlib's own `minizip`, which is a different project with a
# different ABI and no CMake config under that name, so this is built here from
# the same port the Windows build uses -- version and checksum read from it.
build_minizip_ng() {
    local port_dir="${repo_root}/cmake/vcpkg-overlay-ports/minizip-ng"
    local version sha url archive work build config
    version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' \
        "${port_dir}/vcpkg.json")"
    sha="$(sed -n 's/^[[:space:]]*SHA512[[:space:]]\{1,\}\([0-9a-f]\{128\}\).*/\1/p' \
        "${port_dir}/portfile.cmake" | head -n1)"
    [[ -n "$sha" ]] || die "no SHA512 found in ${port_dir}/portfile.cmake"

    if stamped minizip-ng "$version"; then
        echo "== minizip-ng ${version}: up to date"
        return 0
    fi
    echo "== minizip-ng ${version}"

    url="https://github.com/zlib-ng/minizip-ng/archive/refs/tags/${version}.tar.gz"
    archive="${src_dir}/minizip-ng-${version}.tar.gz"
    work="${build_root}/minizip-ng-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$sha"
    unpack "$archive" "${work}/src"

    # `MZ_LIB_SUFFIX=-ng` and `MZ_COMPAT=OFF` are copied from the port: without
    # them the library installs as `libminizip.so`, which collides with zlib's
    # minizip on a distribution that has both.
    configure_build_install "${work}/src" "$build" \
        -DMZ_ZLIB=ON \
        -DMZ_FETCH_LIBS=OFF \
        -DMZ_LIB_SUFFIX=-ng \
        -DMZ_ICONV=OFF \
        -DMZ_COMPAT=OFF \
        -DMZ_BUILD_TESTS=OFF

    config="$(find "${prefix}" \( -name 'minizip-ng-config.cmake' -o -name 'minizip-ngConfig.cmake' \) -print -quit)"
    [[ -n "$config" ]] || die "minizip-ng installed no CMake config package"
    stamp minizip-ng "$version"
}

# --- onnxruntime ------------------------------------------------------------
# The port document (§3 table, §9.3 item 4) says Linux uses the *official
# prebuilt* `libonnxruntime.so` with the CPU execution provider, so unlike the
# three above this is a download, not a build: building ONNX Runtime from source
# is a multi-hour job and is what W9 ("onnxruntime CPU EP 接入") is for.
#
# Two things the prebuilt tarball does not give us:
#   * a CMake config -- `snow_shot/CMakeLists.txt:368` calls
#     `find_package(onnxruntime CONFIG REQUIRED)`, so one is generated here;
#   * a checksum -- Microsoft publishes none, and the 9 MB asset cannot be
#     fetched on this workstation (the proxy stalls at ~300 KB). The pin is
#     therefore version + asset name, and the archive's sha512 is *printed* on
#     first use so it can be filled in below: an unverified pin that says so is
#     better than a `sha512sum` that was never run.
onnxruntime_version="1.28.0"
onnxruntime_sha512_x64=""
onnxruntime_sha512_aarch64=""

build_onnxruntime() {
    local version="$onnxruntime_version"
    local arch sha url archive work target root

    case "$(uname -m)" in
        x86_64|amd64) arch="x64"; sha="$onnxruntime_sha512_x64" ;;
        aarch64|arm64) arch="aarch64"; sha="$onnxruntime_sha512_aarch64" ;;
        *) die "no ONNX Runtime prebuilt for $(uname -m)" ;;
    esac

    if stamped onnxruntime "${version}-${arch}"; then
        echo "== onnxruntime ${version} (${arch}): up to date"
        return 0
    fi
    echo "== onnxruntime ${version} (${arch})"

    url="https://github.com/microsoft/onnxruntime/releases/download/v${version}/onnxruntime-linux-${arch}-${version}.tgz"
    archive="${src_dir}/onnxruntime-linux-${arch}-${version}.tgz"
    work="${build_root}/onnxruntime-${version}-${arch}"

    fetch "$url" "$archive"
    if [[ -n "$sha" ]]; then
        verify "$archive" "$sha"
    else
        echo "warning: no pinned checksum for $(basename "$archive"); sha512 is $(sha512sum "$archive" | cut -d' ' -f1)"
    fi

    rm -rf "$work"
    mkdir -p "$work"
    tar -xzf "$archive" -C "$work" --strip-components=1
    # The release layout is include/ + lib/, which is already the prefix layout.
    cp -a "$work/include/." "${prefix}/include/"
    mkdir -p "${prefix}/lib"
    cp -a "$work/lib/." "${prefix}/lib/"

    root="$(find "${prefix}/lib" -maxdepth 1 -name "libonnxruntime.so.${version}" -print -quit)"
    [[ -n "$root" ]] || die "prebuilt onnxruntime ${version} does not contain libonnxruntime.so.${version}"

    # Minimal config: one imported target, whose location is resolved from this
    # file's own position so the prefix stays relocatable.
    mkdir -p "${prefix}/lib/cmake/onnxruntime"
    cat >"${prefix}/lib/cmake/onnxruntime/onnxruntimeConfig.cmake" <<'CONFIG'
# Generated by scripts/build-linux-deps.sh.
#
# The official ONNX Runtime tarball ships headers and libraries but no CMake
# package, while snow_shot configures with `find_package(onnxruntime CONFIG
# REQUIRED)`. This file is that package: a single imported target, located
# relative to itself so moving the prefix does not break it.
get_filename_component(_snow_ort_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET onnxruntime::onnxruntime)
    add_library(onnxruntime::onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION "${_snow_ort_prefix}/lib/libonnxruntime.so"
        IMPORTED_SONAME "libonnxruntime.so"
        INTERFACE_INCLUDE_DIRECTORIES "${_snow_ort_prefix}/include"
    )
endif()
unset(_snow_ort_prefix)
CONFIG

    config="$(find "${prefix}" -name 'onnxruntimeConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "onnxruntime config was not written"
    stamp onnxruntime "${version}-${arch}"
}

# --- Imath + OpenEXR --------------------------------------------------------
# OpenEXR on 24.04 is not merely "a bit old": `snow_image/src/codecs/exr_codec.cpp`
# is written against the 3.4 API -- `MemoryIStream::size()` overrides
# `IStream::size()`, and opaque attributes are built with
# `OpaqueAttribute(typeName, size, data)` -- while 24.04 ships 3.1.5 (neither
# compiles) and UOS 20 ships 2.5 (different core headers entirely). So the whole
# EXR codec is built here at the version the Windows build uses.
#
# Pins are copied verbatim from `.tools/vcpkg/ports/{openexr,imath}`: same
# version, same SHA512, same archive URL shape as `vcpkg_from_github`, so the
# bytes are identical. `scripts/check-linux-deps-pins.py` re-reads the ports on
# this workstation and fails when they drift apart.
imath_version="3.2.2"
imath_sha512="492a624e4c0b59685d1ea58a3c2c63ddb4ba5ab9177c7d2a1b7e80be95d38ce02c74fafd2fe0982f7d21e5e75c938cc24a33a12d827dec32727cb8dcd5066450"

openexr_version="3.4.9"
openexr_sha512="785496d9b749f2272c108262796abde7ec40f7c328bb91cde9d7ffecf0670826399233aa7bf0db881f0b07748c78660e49b1ca5511fadd0dfe1d9d7bd1fb0f93"

build_imath() {
    local version="$imath_version"
    local url archive work build config

    if stamped imath "$version"; then
        echo "== imath ${version}: up to date"
        return 0
    fi
    echo "== imath ${version}"

    url="https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/imath-${version}.tar.gz"
    work="${build_root}/imath-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$imath_sha512"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DIMATH_INSTALL_PKG_CONFIG=ON \
        -DIMATH_INSTALL_SYM_LINK=OFF

    config="$(find "${prefix}" -name 'ImathConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "imath installed no CMake config package"
    stamp imath "$version"
}

build_openexr() {
    local version="$openexr_version"
    local url archive work build config

    if stamped openexr "$version"; then
        echo "== openexr ${version}: up to date"
        return 0
    fi
    echo "== openexr ${version}"

    url="https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/openexr-${version}.tar.gz"
    work="${build_root}/openexr-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$openexr_sha512"
    unpack "$archive" "${work}/src"

    # `CMAKE_PREFIX_PATH` is what makes it find the Imath installed one step
    # above; without it OpenEXR would either fail or silently use the system
    # 3.1 headers alongside its own 3.4 library. The vcpkg port additionally
    # forces libdeflate (`CMAKE_REQUIRE_FIND_PACKAGE_libdeflate=ON`); this side
    # deliberately does not -- the prefix already carries zlib-ng in compat
    # mode, which is the compression backend Windows uses.
    configure_build_install "${work}/src" "$build" \
        -DCMAKE_PREFIX_PATH="${prefix}" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DBUILD_WEBSITE=OFF \
        -DOPENEXR_BUILD_EXAMPLES=OFF \
        -DOPENEXR_BUILD_TOOLS=OFF \
        -DOPENEXR_INSTALL_TOOLS=OFF \
        -DOPENEXR_INSTALL_PKG_CONFIG=ON

    # `snow_image/CMakeLists.txt` asks for `OpenEXR::OpenEXR`; that target only
    # exists if the config package was installed.
    config="$(find "${prefix}" -name 'OpenEXRConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "openexr installed no CMake config package"
    stamp openexr "$version"
}

# --- libwebp ----------------------------------------------------------------
# Same class of problem as OpenEXR, one API older: `webp_codec.cpp` calls
# `WebPAnimEncoderSetChunk()`, which 24.04's libwebp 1.3.2 does not declare (and
# UOS 20's 0.6 has no animation encoder at all). Windows uses 1.6.0.
#
# The vcpkg port patches `WebPConfig.cmake.in`, but only its legacy
# `WebP_LIBRARIES` variable (absolute vcpkg paths); the imported targets this
# project links -- `WebP::webp`, `WebP::webpdemux`, `WebP::libwebpmux` -- are
# upstream's own, so the patches are not needed here.
libwebp_version="1.6.0"
libwebp_sha512="298e0ad4c09392213baf5abb69d330c6203b618800073fe2df91d01d35034197c5d3e29a74573b06971473c52c74514f0e6e0f6c8162f923e2dd15cb1a692aef"

build_libwebp() {
    local version="$libwebp_version"
    local url archive work build config

    if stamped libwebp "$version"; then
        echo "== libwebp ${version}: up to date"
        return 0
    fi
    echo "== libwebp ${version}"

    url="https://github.com/webmproject/libwebp/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/libwebp-${version}.tar.gz"
    work="${build_root}/libwebp-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$libwebp_sha512"
    unpack "$archive" "${work}/src"

    # Only the three libraries are wanted; the tools would drag in SDL, png
    # helpers and a CLI surface nobody ships.
    configure_build_install "${work}/src" "$build" \
        -DBUILD_SHARED_LIBS=ON \
        -DWEBP_BUILD_LIBWEBPMUX=ON \
        -DWEBP_ENABLE_SIMD=ON \
        -DWEBP_BUILD_ANIM_UTILS=OFF \
        -DWEBP_BUILD_CWEBP=OFF \
        -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_EXTRAS=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF \
        -DWEBP_BUILD_IMG2WEBP=OFF \
        -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF \
        -DWEBP_BUILD_WEBPMUX=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_SDL=ON

    config="$(find "${prefix}" -name 'WebPConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "libwebp installed no CMake config package"
    stamp libwebp "$version"
}

# --- highway + libjxl + libheif ---------------------------------------------
# Both codecs are written against the versions Windows uses, and the gap is not
# a missing symbol here and there:
#   * libjxl 0.11.2 (vcpkg) vs 0.7.0 (24.04): `JXL_DEC_BOX_COMPLETE`,
#     `JxlEncoderDistanceFromQuality` and the `JxlColorProfileTarget` overloads
#     of `JxlDecoderGetColorAs*Profile` all postdate 0.7 -- the codec does not
#     compile at all;
#   * libheif 1.21.2 (vcpkg) vs 1.17.6: `heif_codec.cpp` includes
#     `<libheif/heif_sequences.h>`, which 1.17 does not ship.
# Neither is reachable by adapting the code, and UOS 20 is older still, so the
# same rule as 9.6.12 applies: carry the version.
#
# libjxl needs highway; the port forces the system one (`JPEGXL_FORCE_SYSTEM_HWY`),
# so highway is built right before it. brotli and lcms2 are taken from the
# distribution (the port forces those too) -- see the apt list in §9.1.
#
# libheif is built with exactly the Windows feature set: HEVC via libde265/x265,
# no aom (the overlay port's default features are `hevc` only, so AVIF is absent
# on Windows as well -- building it here would be a *difference*, not parity).
highway_version="1.3.0"
highway_sha512="8b9f4fdc4fa60b6817417959853f5b55bf86aec9d35fc6664dda15179cc55e0a9940f3a46011a84b95263ba342dc47ca1cb93b04481ff4b63d724cce1815d7c6"

libjxl_version="0.11.2"
libjxl_sha512="a7e1f7d060b358f4382e84367d66aa2850aef3b4524a0fdfe3f22dd258fb9e35dda7540f859d8bf4c32f31c61a7a03db677f4490a9f472cd25869a9d00797336"

libheif_version="1.21.2"
libheif_sha512="c953cb9c3a4c0f1052f4d1e0475143cfea6c0a1ab787acb33a2fd173e0460b665717a56a99b864226dfe62bd139c92a66fe5dec75b91e2a333b77a0571e3c5a8"

build_highway() {
    local version="$highway_version"
    local url archive work build config

    if stamped highway "$version"; then
        echo "== highway ${version}: up to date"
        return 0
    fi
    echo "== highway ${version}"

    # The port's REF is the bare version (`REF "${VERSION}"`), not `v${VERSION}`.
    url="https://github.com/google/highway/archive/${version}.tar.gz"
    archive="${src_dir}/highway-${version}.tar.gz"
    work="${build_root}/highway-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$highway_sha512"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DHWY_ENABLE_EXAMPLES=OFF \
        -DHWY_ENABLE_TESTS=OFF \
        -DHWY_ENABLE_CONTRIB=OFF

    config="$(find "${prefix}" -name 'hwy-config.cmake' -o -name 'hwyConfig.cmake' -print -quit)"
    [[ -n "$config" ]] || die "highway installed no CMake config package"
    stamp highway "$version"
}

build_libjxl() {
    local version="$libjxl_version"
    local url archive work build pc

    if stamped libjxl "$version"; then
        echo "== libjxl ${version}: up to date"
        return 0
    fi
    echo "== libjxl ${version}"

    url="https://github.com/libjxl/libjxl/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/libjxl-${version}.tar.gz"
    work="${build_root}/libjxl-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$libjxl_sha512"
    unpack "$archive" "${work}/src"

    # Options are the overlay port's, minus the patches: those teach vcpkg's
    # prefix layout to the bundled find modules, while here every dependency is
    # either in `$prefix` or a distribution package. `FORCE_SYSTEM_*` is what
    # makes it use them instead of the (absent) third_party submodules: a GitHub
    # archive tarball carries no submodules, so skcms/brotli/highway have to come
    # from outside -- that is also why SKCMS is OFF and lcms2 is required.
    configure_build_install "${work}/src" "$build" \
        -DCMAKE_PREFIX_PATH="${prefix}" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DJPEGXL_FORCE_SYSTEM_HWY=ON \
        -DJPEGXL_FORCE_SYSTEM_BROTLI=ON \
        -DJPEGXL_FORCE_SYSTEM_LCMS2=ON \
        -DJPEGXL_ENABLE_SKCMS=OFF \
        -DJPEGXL_BUNDLE_LIBPNG=OFF \
        -DJPEGXL_ENABLE_BENCHMARK=OFF \
        -DJPEGXL_ENABLE_DOXYGEN=OFF \
        -DJPEGXL_ENABLE_EXAMPLES=OFF \
        -DJPEGXL_ENABLE_FUZZERS=OFF \
        -DJPEGXL_ENABLE_JNI=OFF \
        -DJPEGXL_ENABLE_MANPAGES=OFF \
        -DJPEGXL_ENABLE_OPENEXR=OFF \
        -DJPEGXL_ENABLE_PLUGINS=OFF \
        -DJPEGXL_ENABLE_SJPEG=OFF \
        -DJPEGXL_ENABLE_TCMALLOC=OFF \
        -DJPEGXL_ENABLE_TOOLS=OFF

    # `snow_image` finds this codec through pkg-config, not CMake config:
    # `pkg_check_modules(JXL ... libjxl)` + `libjxl_threads`. CMake prepends
    # `<prefix>/lib/pkgconfig` to PKG_CONFIG_PATH for every CMAKE_PREFIX_PATH
    # entry, so the file landing here is what closes the loop.
    pc="$(find "${prefix}" -name 'libjxl.pc' -print -quit)"
    [[ -n "$pc" ]] || die "libjxl installed no libjxl.pc"
    pc="$(find "${prefix}" -name 'libjxl_threads.pc' -print -quit)"
    [[ -n "$pc" ]] || die "libjxl installed no libjxl_threads.pc (JXL_THREADS lookup would fail)"
    stamp libjxl "$version"
}

build_libheif() {
    local version="$libheif_version"
    local url archive work build config

    if stamped libheif "$version"; then
        echo "== libheif ${version}: up to date"
        return 0
    fi
    echo "== libheif ${version}"

    url="https://github.com/strukturag/libheif/archive/refs/tags/v${version}.tar.gz"
    archive="${src_dir}/libheif-${version}.tar.gz"
    work="${build_root}/libheif-${version}"
    build="${work}/cmake-build"

    fetch "$url" "$archive"
    verify "$archive" "$libheif_sha512"
    unpack "$archive" "${work}/src"

    configure_build_install "${work}/src" "$build" \
        -DCMAKE_PREFIX_PATH="${prefix}" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
        -DWITH_LIBDE265=ON \
        -DWITH_X265=ON \
        -DWITH_AOM_DECODER=OFF \
        -DWITH_AOM_ENCODER=OFF \
        -DWITH_DAV1D=OFF \
        -DWITH_JPEG_DECODER=OFF \
        -DWITH_JPEG_ENCODER=OFF \
        -DWITH_OpenH264_DECODER=OFF \
        -DWITH_OpenJPEG_DECODER=OFF \
        -DWITH_OpenJPEG_ENCODER=OFF \
        -DWITH_X264=OFF \
        -DWITH_UNCOMPRESSED_CODEC=OFF \
        -DWITH_LIBSHARPYUV=OFF \
        -DWITH_EXAMPLES=OFF \
        -DWITH_GDK_PIXBUF=OFF \
        -DPLUGIN_DIRECTORY=

    config="$(find "${prefix}" \( -name 'libheif-config.cmake' -o -name 'libheifConfig.cmake' \) -print -quit)"
    [[ -n "$config" ]] || die "libheif installed no CMake config package"
    stamp libheif "$version"
}

for library in zlib libjpeg-turbo lunasvg minizip-ng onnxruntime imath openexr libwebp highway libjxl libheif; do
    if ! selected "$library"; then
        continue
    fi
    log="${log_dir}/${library}.log"
    case "$library" in
        zlib) build_zlib >"$log" 2>&1 || { cat "$log"; die "zlib build failed (log: ${log})"; } ;;
        libjpeg-turbo) build_libjpeg_turbo >"$log" 2>&1 || { cat "$log"; die "libjpeg-turbo build failed (log: ${log})"; } ;;
        lunasvg) build_lunasvg >"$log" 2>&1 || { cat "$log"; die "lunasvg build failed (log: ${log})"; } ;;
        minizip-ng) build_minizip_ng >"$log" 2>&1 || { cat "$log"; die "minizip-ng build failed (log: ${log})"; } ;;
        onnxruntime) build_onnxruntime >"$log" 2>&1 || { cat "$log"; die "onnxruntime install failed (log: ${log})"; } ;;
        imath) build_imath >"$log" 2>&1 || { cat "$log"; die "imath build failed (log: ${log})"; } ;;
        openexr) build_openexr >"$log" 2>&1 || { cat "$log"; die "openexr build failed (log: ${log})"; } ;;
        libwebp) build_libwebp >"$log" 2>&1 || { cat "$log"; die "libwebp build failed (log: ${log})"; } ;;
        highway) build_highway >"$log" 2>&1 || { cat "$log"; die "highway build failed (log: ${log})"; } ;;
        libjxl) build_libjxl >"$log" 2>&1 || { cat "$log"; die "libjxl build failed (log: ${log})"; } ;;
        libheif) build_libheif >"$log" 2>&1 || { cat "$log"; die "libheif build failed (log: ${log})"; } ;;
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
