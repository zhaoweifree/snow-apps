#!/usr/bin/env bash
# 语法检查 Linux 平台源文件（cl.exe 的 /Zs，不产出任何目标文件）。
#
# 本机没有 Linux C++ 交叉工具链，`src/platform/linux/*.cpp` 在 Windows 构建里
# 一行都不会被编译。但这个文件对 Qt 的用法是平台中立的：它只用
# QKeySequence / QGuiApplication / QMetaObject，X11 那部分全在 Rust 里。
# 于是把 Q_OS_LINUX 手动塞给 MSVC，就能把「缺 include、成员名写错、签名不匹配」
# 这类纯语法/类型问题在本机抓出来 —— 剩下真正只能由 UOS 回答的是 ABI 与运行期行为。
#
#   bash scripts/syntax-check-linux-cpp.sh [额外 cl 参数...]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$REPO/.workbuddy/qt5-port/msvc-local/env-msvc-v143.sh" >/dev/null 2>&1 || {
    echo "cannot assemble the MSVC environment" >&2
    exit 1
}

QT="${SNOW_QT_ROOT:-C:/Qt/5.15.2/msvc2019_64}"
REPO_W="$(cygpath -w "$REPO")"
OUT="$REPO/.workbuddy/qt5-port/logs/linux-cpp-syntax-check.log"
mkdir -p "$(dirname "$OUT")"
: >"$OUT"

# Git Bash rewrites anything that looks like a switch into a path ("/nologo" ->
# "D:/.../PortableGit/nologo"), which silently turns every option into an input
# file. Dash-prefixed switches plus this exclusion keep the command line intact.
export MSYS2_ARG_CONV_EXCL='*'

status=0
for file in "$REPO/snow_shot/src/platform/linux/globalshortcutbackend.cpp" \
            "$REPO/snow_shot/src/platform/linux/focusedfullscreenwindow.cpp" \
            "$REPO/snow_shot/src/platform/linux/globalmousebackend.cpp" "$@"; do
    echo "=== $(basename "$file")" | tee -a "$OUT"
    cl -nologo -Zs -std:c++20 -EHsc -W4 -permissive- \
        -DWIN32 -D_WINDOWS -DQ_OS_LINUX -DUNICODE -D_UNICODE \
        "-I$REPO_W/snow_shot/include" \
        "-I$REPO_W/snow-crates/crates/snow-linux-c/include" \
        "-I$QT/include" \
        "-I$QT/include/QtCore" \
        "-I$QT/include/QtGui" \
        "-I$QT/include/QtWidgets" \
        "$(cygpath -w "$file")" 2>&1 | tee -a "$OUT"
    if [ "${PIPESTATUS[0]}" -ne 0 ]; then
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "SYNTAX OK ($OUT)"
else
    echo "SYNTAX FAILED ($OUT)"
fi
exit "$status"
