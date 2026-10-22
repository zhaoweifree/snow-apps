#!/bin/bash
# Local gate for the Linux packaging files (port document §9.5).
#
# Two halves, because neither tool can do the other's job:
#   * cmake -P evaluates `cmake/SnowShotLinuxPackage.cmake` itself -- the module is
#     only included on Linux, so nothing else would ever parse it on a Windows
#     host, and an architecture mapping typo would survive until a real configure;
#   * the Python half checks the freedesktop files the module installs, which no
#     CMake construct can validate (XML well-formedness, io.github-style rules,
#     and the cross-file agreement on the application id).
#
# Runs on any host: no Linux toolchain, no dpkg, no appstreamcli required.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

cmake_command="${CMAKE_COMMAND:-}"
if [[ -z "$cmake_command" ]]; then
    if command -v cmake >/dev/null 2>&1; then
        cmake_command="cmake"
    elif [[ -x "C:/snow-tools/cmake/cmake-4.2.3-windows-x86_64/bin/cmake.exe" ]]; then
        cmake_command="C:/snow-tools/cmake/cmake-4.2.3-windows-x86_64/bin/cmake.exe"
    else
        echo "cmake not found; set CMAKE_COMMAND" >&2
        exit 2
    fi
fi

python_command="${PYTHON:-}"
if [[ -z "$python_command" ]]; then
    if [[ -n "${SNOW_PYTHON:-}" ]]; then
        python_command="$SNOW_PYTHON"
    elif command -v python3 >/dev/null 2>&1; then
        python_command="python3"
    else
        python_command="python"
    fi
fi

echo "=== packaging module"
"$cmake_command" -P scripts/check-linux-packaging.cmake

echo
echo "=== freedesktop integration"
"$python_command" scripts/check-linux-freedesktop.py

echo
echo "=== hicolor icon tree"
"$python_command" scripts/gen-linux-icons.py --check

echo
echo "=== appimage bundle"
"$python_command" scripts/check-linux-appimage.py

echo
echo "=== linux ci workflow"
"$python_command" scripts/check-linux-ci-workflow.py

echo
echo "=== qt5 install path resolution"
# Runs on any host: it builds a fake multiarch layout and a fake prefix layout,
# then asserts the resolver picks the right one for each (9.6.8).
"$cmake_command" -P scripts/check-qt5-install-paths.cmake

echo
echo "=== cmake module self-reference scope"
# Textual on purpose: `CMAKE_CURRENT_LIST_DIR` inside a function body resolves
# against the *caller*, and a `cmake -P` fixture always calls at file scope, so
# no runtime check can catch this class (9.6.10).
"$python_command" scripts/check-cmake-list-dir-scope.py

echo
echo "=== qt private module pairing"
# Every `${SNOW_QT_NS}::<Module>Private` link needs a `snow_find_qt_private(Module)`
# in the same CMakeLists: on Qt5 that call is what creates the target (9.6.11).
"$python_command" scripts/check-qt-private-modules.py

echo
echo "=== linux dependency pins"
# The imath/openexr/libwebp pins are *copies* of vcpkg builtin ports (CI has no
# clone). A stale copy compiles an old API against a newer snow_image (9.6.12).
"$python_command" scripts/check-linux-deps-pins.py

echo
echo "=== ffmpeg wiring"
# ffmpeg-sys-next resolves FFmpeg through FFMPEG_DIR *before* pkg-config and
# then looks under <FFMPEG_DIR>/include only, so one stale prefix turns a
# machine with libavfilter-dev installed into a build that cannot find the
# headers (run 13, 9.6.16).
"$python_command" scripts/check-linux-ffmpeg-wiring.py

echo
echo "=== qt compat macro order"
# The compat header is force-included into every C++ TU, and it used to test
# SNOW_QT5 before defining it: an undefined macro in #if evaluates to 0, so
# the Qt5 branch was dead code on every platform and -Wundef turned it into
# a hard error on GCC (run 15, 9.6.17).
"$python_command" scripts/check-qt-compat-macro-order.py
