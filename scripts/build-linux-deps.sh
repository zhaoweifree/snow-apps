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
#     and the second lacks `<libheif/heif_sequences.h>`;
#   * OpenCV -- the barcode service is written against OpenCV 4.12's
#     `cv::barcode` (which lives in the *main* `objdetect` module since 4.9)
#     and against the in-memory-model `WeChatQRCode` constructor that this
#     repository adds with its own patch; 24.04 ships 4.6, whose contrib
#     `barcode` is a different API, and the patch exists only in the vcpkg
#     overlay port -- see the long comment above `build_opencv`.
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
# `opencv` is the one port that fetches *two* archives (the main repository
# and `opencv_contrib`, which carries the `wechat_qrcode` module) and a patch
# file besides, so "the first SHA512 in the portfile" is the *patch's*
# checksum. Its pins are therefore compared per archive, by the repository
# the `vcpkg_from_github` call names -- again in `check-linux-deps-pins.py`.
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

# fd 3 = the console's stderr, kept across the per-library redirections so `die`
# stays visible (see die()).
exec 3>&2

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
    # `die` is called from inside builders whose stdout *and* stderr are
    # redirected into `<prefix>/logs/<lib>.log`, which used to make a failed
    # assertion invisible: the message went into the log file, the shell exited,
    # and the loop's `cat` never ran (CI run 39607822963 lost the highway error
    # exactly this way, leaving only "Process completed with exit code 1").
    # fd 3 is a saved copy of the real stderr, so the message always surfaces.
    #
    # `exit` ends the *whole* shell, which is why every builder now runs in a
    # subshell (see the loop at the bottom): inside a subshell `die` ends the
    # builder only, the loop's error branch runs, and the log is dumped. CI run
    # 39764499751 lost the OpenCV error exactly this way -- the step said
    # "opencv installed no OpenCVConfig.cmake" and nothing else, for a build
    # that had failed somewhere else entirely.
    echo "error: $*" >&3
    exit 1
}

# Dumps the tail of a builder's log plus the lines that look like errors, to
# the real stderr. The log file itself lives on the runner's disk and is gone
# when the job ends, so this is the only copy of the reason a build failed.
dump_log() { # log-file
    local log="$1"
    [[ -f "$log" ]] || return 0
    echo "----- last 150 lines of ${log} -----" >&3
    tail -n 150 "$log" >&3
    echo "----- error lines in ${log} -----" >&3
    grep -n -i -E "cmake error|fatal|error:|error [0-9]+|no space left|failed" "$log" \
        | tail -n 40 >&3
    true
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

# The runner's temporary directory is a finite disk and the build trees of
# libjxl, libheif and OpenCV are large. A disk that fills up fails in the
# middle of `tar` or `cmake`, and from the outside that is indistinguishable
# from a build bug -- so the free space is reported before every library and
# checked against what that library needs.
report_free_space() { # label
    local label="$1" avail
    avail="$(df -Pm "${prefix}" 2>/dev/null | awk 'NR==2 {print $4}')"
    if [[ -n "$avail" ]]; then
        echo "   disk before ${label}: ${avail} MiB free under ${prefix}"
    fi
}

require_free_space() { # label minimum-mib
    local label="$1" need="$2" avail
    avail="$(df -Pm "${prefix}" 2>/dev/null | awk 'NR==2 {print $4}')"
    [[ -n "$avail" ]] || return 0
    if (( avail < need )); then
        die "not enough disk space for ${label}: ${avail} MiB free under ${prefix}, need ${need} MiB"
    fi
}

# The build tree is scratch space: the stamp makes a rebuild a no-op, and the
# downloaded archives stay in `<prefix>/src`, so keeping the tree only eats
# the disk. Removing it after each library keeps the peak usage at one
# library instead of all twelve.
prune_build_tree() { # label
    local label="$1" dir
    for dir in "${build_root}/${label}-"*; do
        [[ -d "$dir" ]] || continue
        rm -rf "$dir"
    done
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
    # Every helper below checks its own status instead of relying on
    # `set -e`: a builder runs on the left-hand side of a `||` (so that the
    # log can be dumped), and bash turns `errexit` off for the whole
    # left-hand side of `||`. Without these checks a failed download or a
    # failed `tar` would be ignored and the build would carry on with a
    # half-written tree.
    curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors -m 900 \
        -o "$archive.part" "$url" \
        || die "download failed: ${url}"
    mv "$archive.part" "$archive" \
        || die "cannot move the downloaded archive into place: ${archive}"
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
    tar -xzf "$archive" -C "$work" --strip-components=1 \
        || die "cannot unpack $(basename "$archive")"
}

# Applies a unified diff to a source tree. `patch` is the classic tool and is
# what the vcpkg ports' patches are written for; `git apply` is the fallback
# for images that do not ship it. Both must *fail* when the diff does not
# apply: a silently skipped patch is how a build ends up compiling a source
# tree that is not the one the project pinned.
apply_patch() { # directory patch-file
    local directory="$1" patch_file="$2"
    if command -v patch >/dev/null 2>&1; then
        patch --batch --forward -p1 -d "$directory" <"$patch_file" \
            || die "the patch does not apply: ${patch_file}"
    elif command -v git >/dev/null 2>&1; then
        git -C "$directory" apply -p1 --verbose "$patch_file" \
            || die "the patch does not apply: ${patch_file}"
    else
        die "neither 'patch' nor 'git' is available to apply ${patch_file}"
    fi
}

configure_build_install() { # source build-dir extra cmake args...
    local source="$1" build="$2"
    shift 2
    # The three steps are checked one by one and each failure names the step
    # it came from. `die` (rather than `return`) is what stops the builder:
    # inside the builder's subshell it ends that subshell, so the loop can
    # dump the log and the reason never gets lost behind a later assertion.
    "$cmake_command" -S "$source" -B "$build" ${generator[@]+"${generator[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        "$@" \
        || die "cmake configure failed: ${source}"
    "$cmake_command" --build "$build" --parallel "$jobs" \
        || die "cmake build failed: ${source}"
    "$cmake_command" --install "$build" \
        || die "cmake install failed: ${source}"
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
# Two of the things the prebuilt tarball does not give us:
#   * a CMake config -- `snow_shot/CMakeLists.txt:368` calls
#     `find_package(onnxruntime CONFIG REQUIRED)`, so one is generated here;
#   * a checksum -- Microsoft publishes none. The pins below were read off the
#     archive the first real run downloaded on each architecture (run
#     39607822963), which is the only honest way to obtain them: they are
#     checksums of the exact assets, verified from then on by `sha512sum`.
onnxruntime_version="1.28.0"
onnxruntime_sha512_x64="4191bc612e14ab7dc28f76002bd21ad7a1aa981cd45f9572045f081d7d3947ab49c1918131d27fdb0482d389972b03ed5e21b268c4f2aacd63f9eb4ea3554bd3"
onnxruntime_sha512_aarch64="6fb4fbdd45517beeb9f19a42291184122bc25b92fe4854e487f3b047d747469460d5ada15f13308934f399c5149d1b82725469de1805dd4b2f9658bb8a0d1f59"

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
        -DHWY_ENABLE_INSTALL=ON \
        -DHWY_ENABLE_EXAMPLES=OFF \
        -DHWY_ENABLE_TESTS=OFF \
        -DHWY_ENABLE_CONTRIB=OFF

    # `HWY_ENABLE_INSTALL` is what produces this file at all -- upstream leaves
    # installation off by default, so without it the target libjxl links simply
    # does not exist.
    config="$(find "${prefix}" -name 'hwy-config.cmake' -print -quit)"
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

# --- OpenCV + opencv_contrib -----------------------------------------------
# The barcode recognition service is the only OpenCV consumer, and it is
# written against two APIs that Ubuntu 24.04 does not have:
#
#   * `cv::barcode::BarcodeDetector::decodeWithType()` -- OpenCV moved the
#     barcode detector out of contrib and into the main `objdetect` module in
#     4.9. 24.04 ships 4.6, where the contrib `barcode` module has a
#     *different* API (`decode()` plus a `BarcodeType` enum) and lives in
#     `opencv_contrib` rather than `opencv_objdetect`;
#   * `WeChatQRCode`'s four-argument constructor that takes the models as
#     `std::vector<uchar>` -- which is **not** an upstream API at any version:
#     it is added by this repository's own patch
#     `cmake/vcpkg-overlay-ports/opencv4/0024-contrib-wechat-qrcode-in-memory-models.patch`,
#     so that the models can be loaded through a platform-aware API instead
#     of a narrow-character path (Windows install directories that the
#     active ANSI code page cannot represent).
#
# Adapting the *code* to 4.6 would mean dropping 1D barcodes or hand-writing
# an enum-to-string mapping, and it would take the Unicode-path capability
# away on Linux only -- the two platforms would stop being the same program.
# So the distribution package is not an option: OpenCV 4.12.0 and
# opencv_contrib 4.12.0 are built here, with the *same* patch the vcpkg
# overlay port applies, so both sides compile the same code against the same
# API.
#
# The build is **static**, for three reasons:
#   * it matches the Windows side, where the overlay port is
#     `vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`;
#   * the .deb has to run on UOS 20, whose OpenCV is older still -- a shared
#     `libopencv_core.so.4.12` would have to be shipped inside the package
#     and would then be a shared library no package owns;
#   * `dpkg-shlibdeps` derives the package's Depends from the ELF NEEDED
#     entries, and a shared OpenCV that belongs to no distribution package
#     makes that step fail rather than warn.
#
# The module set is narrowed to `wechat_qrcode` exactly like the overlay port
# (`-DBUILD_LIST=wechat_qrcode`), so the build pulls in core, imgproc,
# objdetect, dnn and the contrib module -- and nothing else.
opencv_version="4.12.0"
opencv_sha512="8ac63ddd61e22cc0eaeafee4f30ae6e1cab05fc4929e2cea29070203b9ca8dfead12cc0fd7c4a87b65c1e20ec6b9ab4865a1b83fad33d114fc0708fdf107c51b"
opencv_contrib_version="4.12.0"
opencv_contrib_sha512="574121ca57328671741413df91fbf600cc04bb9a9beeacfb7bc20c15b2b4e8c9e031df30aafbcc34f82d85edfb098e5d008a744f4e6d833d6e47537a042045c6"

# The four WeChat QR models, with the SHA512 the project's own
# `cmake/FetchSnowShotQrModels.cmake` pins and the MD5 OpenCV's download cache
# is keyed by (`<md5>-<filename>`). `scripts/check-linux-deps-pins.py` compares
# the two lists, because a model that differs between the two sides is a
# barcode that decodes on Windows and not on Linux.
wechat_qrcode_model_url="https://raw.githubusercontent.com/WeChatCV/opencv_3rdparty/a8b69ccc738421293254aec5ddb38bd523503252"
wechat_qrcode_models="detect.caffemodel|58d62faf8679d3f568a26a1d9f7c2e88060426a440315ca8bce7b3b5a8efa34be670afd0abfd0dd5d89f89a042a2408ea602f937080abc6910c2e497b7f5a4b8|238e2b2d6f3c18d6c3a30de0c31e23cf
sr.caffemodel|917c6f6b84a898b8c8c85c79359e48a779c8a600de563dac2e1c5d013401e9ac9dbcd435013a4ed7a69fc936839fb189aaa3038c127d04ceb6fd3b8fd9dd67bd|cbfcd60361a73beb8c583eea7e8e6664
detect.prototxt|2239d31a597049f358f09dbb4c0a7af0b384d9b67cfa3224f8c7e44329647cf19ee7929ac06199cca23bbbf431de0481b74ab51eace6aa20bb2e2fd19b536e49|6fb4976b32695f9f5c6305c19f12537d
sr.prototxt|6b715ec45c3fd081e7e113e351edcef0f3d32a75f8b5a9ca2273cb5da9a1116a1b78cba45582a9acf67a7ab76dc4fcdf123f7b3a0d3de2f5c39b26ef450058b7|69db99927a70df953b471daaba03fbef"

build_opencv() {
    local version="$opencv_version"
    local url archive contrib_url contrib_archive work build contrib_src
    local patch_file download_cache name sha md5 model_archive actual_md5

    if stamped opencv "$version"; then
        echo "== opencv ${version}: up to date"
        return 0
    fi
    echo "== opencv ${version} + opencv_contrib ${opencv_contrib_version}"

    url="https://github.com/opencv/opencv/archive/refs/tags/${version}.tar.gz"
    archive="${src_dir}/opencv-${version}.tar.gz"
    contrib_url="https://github.com/opencv/opencv_contrib/archive/refs/tags/${opencv_contrib_version}.tar.gz"
    contrib_archive="${src_dir}/opencv_contrib-${opencv_contrib_version}.tar.gz"
    work="${build_root}/opencv-${version}"
    build="${work}/cmake-build"
    contrib_src="${work}/contrib-src"
    patch_file="${repo_root}/cmake/vcpkg-overlay-ports/opencv4/0024-contrib-wechat-qrcode-in-memory-models.patch"
    download_cache="${work}/downloads"

    fetch "$url" "$archive"
    verify "$archive" "$opencv_sha512"
    fetch "$contrib_url" "$contrib_archive"
    verify "$contrib_archive" "$opencv_contrib_sha512"

    unpack "$archive" "${work}/src"
    unpack "$contrib_archive" "$contrib_src"

    # The same patch the vcpkg overlay port applies, on the same tree layout:
    # the diff's paths are `modules/wechat_qrcode/...`, which is the layout of
    # the opencv_contrib source root. If it does not apply, the build has to
    # stop here -- without it the four-argument constructor does not exist and
    # the barcode service does not compile, which is the failure this whole
    # step exists to prevent.
    [[ -f "$patch_file" ]] || die "the in-memory-model patch is missing: ${patch_file}"
    apply_patch "$contrib_src" "$patch_file"

    # The contrib module downloads its four models while *configuring*. They
    # are fetched here instead -- same URLs, same SHA512 as
    # `cmake/FetchSnowShotQrModels.cmake` -- and dropped into OpenCV's own
    # download cache under the names `ocv_download` looks for
    # (`<md5>-<filename>` below `<download path>/wechat_qrcode/`), so the
    # configure step never touches the network and never waits out a
    # 600-second download timeout.
    mkdir -p "${download_cache}/wechat_qrcode"
    while IFS='|' read -r name sha md5; do
        [[ -n "$name" ]] || continue
        model_archive="${src_dir}/wechat-qrcode-models/${name}"
        fetch "${wechat_qrcode_model_url}/${name}" "$model_archive"
        verify "$model_archive" "$sha"
        # The MD5 is the *cache key* OpenCV looks the file up by, so a wrong
        # one does not fail the build -- it silently sends the configure step
        # back to the network. Checking it here turns that into a loud error.
        actual_md5="$(md5sum "$model_archive" | cut -d' ' -f1)"
        [[ "$actual_md5" == "$md5" ]] \
            || die "QR model ${name} has md5 ${actual_md5}, expected ${md5}"
        cp -f "$model_archive" "${download_cache}/wechat_qrcode/${md5}-${name}"
    done <<<"$wechat_qrcode_models"

    # The options are the overlay port's, minus the ones that only make sense
    # inside vcpkg:
    #   * the vcpkg patches are Windows- or vcpkg-specific (`0001` replaces
    #     every download with a fatal error, `0008` swaps the in-tree quirc
    #     for vcpkg's), so they are not applied here;
    #   * `0009` forces `find_package(Protobuf CONFIG REQUIRED)`, which is
    #     the opposite of what this build wants: the bundled protobuf is
    #     built from OpenCV's own sources (`-DBUILD_PROTOBUF=ON`);
    #   * the quirc module stays in-tree -- Ubuntu 24.04 has no `quirc`
    #     package to swap it for;
    #   * `OPENCV_DOWNLOAD_PATH` points at the cache the models were just
    #     placed in.
    #
    # `ENABLE_CONFIG_VERIFICATION=ON` is the port's, and it is the reason for
    # the long `WITH_*` list below. At the end of the configure OpenCV runs
    # `ocv_verify_config()` (cmake/OpenCVUtils.cmake): every option that has a
    # `VERIFY` clause is compared against the flag that says whether the
    # dependency was really found, and a disagreement is a `FATAL_ERROR`.
    # Run 19 died exactly there and only said "opencv installed no
    # OpenCVConfig.cmake", because that assertion is the next statement.
    #
    # Two properties of `OCV_OPTION` decide what has to be written here:
    #
    #   * the `VERIFY` registration sits outside `if(${__condition})`, so
    #     options that are invisible on Linux are registered too -- but they
    #     are never `option()`-declared, their value expands to nothing and
    #     neither branch of the verification fires. They need no entry;
    #   * every dependency probe is guarded by its own option, and the
    #     videoio/highgui guards live in `modules/<m>/cmake/init.cmake`,
    #     which `ocv_glob_modules()` includes for *every* module in the first
    #     pass -- being outside `BUILD_LIST` does not stop a detection. So an
    #     option that is OFF is always consistent, while an option that is ON
    #     with a missing dependency always fails.
    #
    # Only the second case has to be spelled out: options that default to ON
    # and whose dependency this image does not provide. Replaying the upstream
    # `OCV_OPTION` macro and `ocv_verify_config()` against the apt list of
    # `.github/workflows/snow-shot-linux.yml` names exactly sixteen of them,
    # and `scripts/check-linux-deps-opencv-options.py` keeps that set honest.
    #
    # `WITH_TESSERACT` is one of those, and it is not in OpenCV's own tree:
    # `opencv_contrib/modules/text/cmake/init.cmake` declares it with
    # `VERIFY HAVE_TESSERACT` and a default of `(NOT CMAKE_CROSSCOMPILING)` --
    # ON for a native build. That file is included even though the text module
    # is not built, because `ocv_glob_modules()` runs every module's
    # `cmake/init.cmake` in its first pass. It was the last mismatch that run
    # 20 reported, and the only one left in a log that finally showed the real
    # cmake error.
    #
    # `WITH_FFMPEG` is deliberately *not* turned off: the CI installs
    # libavcodec/libavformat/libavutil/libswscale, `detect_ffmpeg.cmake` finds
    # them through pkg-config, so the flag is true and `=OFF` would be the
    # mismatch instead. Nothing links FFMPEG, because videoio is not in the
    # build list. `WITH_V4L` is off although its probe (the kernel headers)
    # would succeed -- turning it off costs nothing here and keeps the option
    # set independent of the runner's kernel headers.
    #
    # `WITH_ADE=OFF` is a different kind of trap, and the reason it is written
    # here even though `ocv_verify_config()` never looks at it:
    # `modules/gapi/cmake/init.cmake` declares
    # `OCV_OPTION(WITH_ADE "..." ON)` -- no `VERIFY` clause, default ON, and
    # the file runs in `ocv_glob_modules()`'s first pass, so it applies even
    # though gapi is not in `BUILD_LIST`. With the option on, `DownloadADE.cmake`
    # fetches the framework and creates an `ade` target that gets exported from
    # `OpenCVModules.cmake`, while the archive itself is never installed --
    # gapi is what would build it. Every `find_package(OpenCV CONFIG)` then
    # aborts with "The imported target ade references the file
    # .../libade.a but this file does not exist" (run 21). The vcpkg port
    # passes the same `-DWITH_ADE=OFF` for the same reason.
    #
    # `WITH_IPP=OFF` also keeps the build hermetic: with the option ON,
    # OpenCV downloads the IPPICV package during the configure step
    # (cmake/OpenCVFindIPP.cmake), which is a network access in the middle of
    # a build that is supposed to be offline.
    #
    # `OPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION` must stay FALSE (it is written
    # out rather than left to the default so that it cannot drift back). With
    # it TRUE, `cmake/OpenCVDetectCXXCompiler.cmake` skips the block that sets
    # `X86_64` / `AARCH64` from `CMAKE_SYSTEM_PROCESSOR` -- the comment there
    # says those variables are then "passed through cache / CMake's command
    # line", which is for cross builds and which nothing here does. Without
    # them `OpenCVCompilerOptimizations.cmake` matches neither the x86 nor the
    # ARM branch, so `CPU_DISPATCH` stays empty and the fallback sets
    # `CPU_BASELINE=DETECT`; on aarch64 DETECT enables NEON in the baseline,
    # which defines `CV_TRY_NEON`, so the dnn convolution code calls
    # `opt_NEON::convBlock_F32` -- an implementation that only exists in a
    # dispatched translation unit, and no dispatched unit was generated:
    #
    #   undefined reference to `cv::dnn::opt_NEON::convBlock_F32(int, ...)`
    #   undefined reference to `cv::dnn::opt_NEON::convBlockMR1_F32(int, ...)`
    #       (CI run 29, arm64 link)
    #
    # A static library does not report unresolved symbols, so the archive was
    # "built" a dozen runs ago and the defect only surfaced when the executable
    # finally linked. With detection on, aarch64 gets the upstream defaults
    # (`CPU_BASELINE=DETECT`, `CPU_DISPATCH=NEON_FP16;NEON_BF16;NEON_DOTPROD`)
    # and x64 gets `CPU_BASELINE=SSE3` plus the usual dispatch list, which is
    # also what makes the shipped archive portable instead of tuned to the
    # runner's CPU.
    configure_build_install "${work}/src" "$build" \
        -DCMAKE_PREFIX_PATH="${prefix}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DOPENCV_EXTRA_MODULES_PATH="${contrib_src}/modules" \
        -DOPENCV_DOWNLOAD_PATH="${download_cache}" \
        -DBUILD_LIST=wechat_qrcode \
        -DBUILD_PROTOBUF=ON \
        -DWITH_PROTOBUF=ON \
        -DWITH_FLATBUFFERS=OFF \
        -DBUILD_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_PERF_TESTS=OFF \
        -DBUILD_DOCS=OFF \
        -DBUILD_IPP_IW=OFF \
        -DBUILD_ITT=OFF \
        -DBUILD_JASPER=OFF \
        -DBUILD_JPEG=OFF \
        -DBUILD_OPENEXR=OFF \
        -DBUILD_OPENJPEG=OFF \
        -DBUILD_PNG=OFF \
        -DBUILD_TIFF=OFF \
        -DBUILD_WEBP=OFF \
        -DBUILD_ZLIB=OFF \
        -DBUILD_TBB=OFF \
        -DBUILD_opencv_apps=OFF \
        -DBUILD_opencv_java=OFF \
        -DBUILD_opencv_js=OFF \
        -DBUILD_JAVA=OFF \
        -DBUILD_ANDROID_PROJECT=OFF \
        -DBUILD_ANDROID_EXAMPLES=OFF \
        -DBUILD_PACKAGE=OFF \
        -DCMAKE_CXX_STANDARD=17 \
        -DOPENCV_CONFIG_INSTALL_PATH=share/opencv4 \
        -DOPENCV_INCLUDE_INSTALL_PATH=include/opencv4 \
        -DOPENCV_LIB_INSTALL_PATH=lib \
        -DOPENCV_3P_LIB_INSTALL_PATH=lib/manual-link/opencv4_thirdparty \
        -DENABLE_CONFIG_VERIFICATION=ON \
        -DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=FALSE \
        -DWITH_ADE=OFF \
        -DWITH_AVIF=OFF \
        -DWITH_CUDA=OFF \
        -DWITH_CUDNN=OFF \
        -DWITH_EIGEN=OFF \
        -DWITH_FASTCV=OFF \
        -DWITH_GSTREAMER=OFF \
        -DWITH_IPP=OFF \
        -DWITH_JASPER=OFF \
        -DWITH_LAPACK=OFF \
        -DWITH_OPENCL=OFF \
        -DWITH_OPENCLAMDFFT=OFF \
        -DWITH_OPENCLAMDBLAS=OFF \
        -DWITH_OPENCL_D3D11_NV=OFF \
        -DWITH_OPENJPEG=OFF \
        -DWITH_V4L=OFF \
        -DWITH_VA=OFF \
        -DWITH_VA_INTEL=OFF \
        -DWITH_VTK=OFF \
        -DWITH_QT=OFF \
        -DWITH_GTK=OFF \
        -DWITH_TBB=OFF \
        -DWITH_ITT=OFF \
        -DWITH_SPNG=OFF \
        -DWITH_ZLIB_NG=OFF \
        -DWITH_CPUFEATURES=OFF \
        -DWITH_TESSERACT=OFF

    # The consumer's view, not the build's: `find_package(OpenCV CONFIG
    # REQUIRED)` reads the config package, and the two headers the barcode
    # service includes have to be there. A build that produced the libraries
    # but not the config would still "succeed" here and fail three steps
    # later in the project's own configure.
    [[ -f "${prefix}/share/opencv4/OpenCVConfig.cmake" ]] \
        || die "opencv installed no OpenCVConfig.cmake"
    [[ -f "${prefix}/include/opencv4/opencv2/objdetect.hpp" ]] \
        || die "opencv installed no objdetect header"
    [[ -f "${prefix}/include/opencv4/opencv2/wechat_qrcode.hpp" ]] \
        || die "opencv_contrib installed no wechat_qrcode header"

    # The two APIs the service is written against, checked in the *installed*
    # headers. This is the product result, not a proxy for it: a 4.6 install
    # would have every file above and still fail to compile.
    grep -q 'decodeWithType' "${prefix}/include/opencv4/opencv2/objdetect/barcode.hpp" \
        || die "the installed objdetect has no decodeWithType (cv::barcode is not the 4.9+ one)"
    grep -q 'const std::vector<uchar>& detector_prototxt_buffer' \
        "${prefix}/include/opencv4/opencv2/wechat_qrcode.hpp" \
        || die "the installed wechat_qrcode has no in-memory-model constructor (the patch did not take)"

    stamp opencv "$version"
}

for library in zlib libjpeg-turbo lunasvg minizip-ng onnxruntime imath openexr libwebp highway libjxl libheif opencv; do
    if ! selected "$library"; then
        continue
    fi
    log="${log_dir}/${library}.log"
    space_needed=1024
    case "$library" in
        zlib) build_fn=build_zlib ;;
        libjpeg-turbo) build_fn=build_libjpeg_turbo ;;
        lunasvg) build_fn=build_lunasvg ;;
        minizip-ng) build_fn=build_minizip_ng ;;
        onnxruntime) build_fn=build_onnxruntime ;;
        imath) build_fn=build_imath ;;
        openexr) build_fn=build_openexr ;;
        libwebp) build_fn=build_libwebp ;;
        highway) build_fn=build_highway ;;
        libjxl) build_fn=build_libjxl; space_needed=3072 ;;
        libheif) build_fn=build_libheif ;;
        opencv) build_fn=build_opencv; space_needed=4096 ;;
    esac

    report_free_space "$library"
    require_free_space "$library" "$space_needed"

    # The builder runs in a subshell, so `die` inside it ends that subshell
    # only and the error branch below can still dump the log before the
    # script exits. Without the subshell the first `die` takes the whole
    # script with it and the log is never printed -- which is how the OpenCV
    # failure of CI run 39764499751 came down to a single misleading line.
    ( "$build_fn" ) >"$log" 2>&1 \
        || { dump_log "$log"; die "${library} build failed (log: ${log})"; }

    echo "   $(tail -n 1 "$log")"
    prune_build_tree "$library"
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
