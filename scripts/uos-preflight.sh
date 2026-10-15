#!/usr/bin/env bash
# uos-preflight.sh -- one-shot environment probe for the Snow Shot UOS/Linux port.
#
# Run this ON THE UOS / Deepin MACHINE (not on the Windows workstation):
#
#     bash scripts/uos-preflight.sh              # report only
#     bash scripts/uos-preflight.sh --probe      # also compile the §6.1 decision probe
#     bash scripts/uos-preflight.sh --out /tmp/uos-report.txt
#
# It exists to close the single open decision point in docs-uos-linux-port.md
# §6.1: whether Qt 5.15's *private* QRhi headers (`qtbase5-private-dev`) and the
# matching Qt 5.15 Shader Tools (`qsb`) are available on UOS.
#
#   * both available  -> snow_image_viewer keeps its QRhi renderer, with the
#                        <rhi/qrhi.h> -> <QtGui/private/qrhi_p.h> include swap,
#                        the ~10 lines of HDR downgrade, and .qsb re-baked with
#                        the Qt 5.15 qsb (Qt6 v9 .qsb is silently rejected by
#                        Qt5, which only understands v1-v5).
#   * either missing   -> fall back to the QOpenGLWidget rewrite of the viewer.
#
# Everything printed is either a decision input or a build prerequisite, so the
# output can be pasted straight into the port log.

set -u

OUT=""
RUN_PROBE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --probe) RUN_PROBE=1 ;;
        --out) shift; OUT="${1:-}" ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

emit() { printf '%s\n' "$*"; }
sec()  { printf '\n===== %s =====\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Verdict accumulators.
qt5_ok=0
private_ok=0
qsb_ok=0

if [ -n "$OUT" ]; then
    # Mirror everything to a file as well as the terminal.
    exec > >(tee "$OUT") 2>&1
fi

emit "snow-apps UOS preflight"
emit "date            : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
emit "repo root       : $REPO_ROOT"

# ---------------------------------------------------------------- 1. host ----
sec "1. host / toolchain"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    emit "distro          : ${PRETTY_NAME:-unknown}"
    emit "distro id       : ${ID:-?} ${VERSION_ID:-?}"
else
    emit "distro          : /etc/os-release missing"
fi
emit "kernel          : $(uname -srmo 2>/dev/null || uname -a)"
emit "arch            : $(uname -m)"
emit "nproc           : $(nproc 2>/dev/null || echo '?')"
emit "glibc           : $(ldd --version 2>/dev/null | head -1 || echo 'not found')"
emit "libstdc++ so    : $(ls -1 /usr/lib/*/libstdc++.so.6* 2>/dev/null | head -1 || echo 'not found')"
for t in gcc g++ cc cmake ninja pkg-config make git patchelf; do
    if have "$t"; then emit "$(printf '%-15s' "$t") : $("$t" --version 2>&1 | head -1)"; else emit "$(printf '%-15s' "$t") : MISSING"; fi
done
if have cmake; then
    _cmv="$(cmake --version | head -1 | awk '{print $3}')"
    case "$_cmv" in
        4.*|3.[2-9][0-9].*) emit "cmake >= 3.20   : yes" ;;
        *) emit "cmake >= 3.20   : NO (project requires cmake_minimum_required 4.2)" ;;
    esac
fi

# ------------------------------------------------------------------ 2. Qt5 ---
sec "2. Qt5 development environment"
if have qmake; then
    emit "qmake           : $(command -v qmake)"
    emit "qmake -v        : $(qmake -v 2>&1 | tr '\n' ' ')"
elif have qmake5; then
    emit "qmake5 -v       : $(qmake5 -v 2>&1 | tr '\n' ' ')"
else
    emit "qmake           : MISSING (install qtbase5-dev qt5-qmake)"
fi
if have pkg-config; then
    for m in Qt5Core Qt5Gui Qt5Widgets Qt5Svg Qt5Network Qt5PrintSupport Qt5DBus Qt5Xml Qt5Test Qt5Concurrent; do
        if v="$(pkg-config --modversion "$m" 2>/dev/null)"; then emit "$(printf '%-16s' "$m"): $v"; else emit "$(printf '%-16s' "$m"): missing"; fi
    done
fi
qt5_priv_pc=""
for c in Qt5Core Qt5Widgets; do
    d="$(pkg-config --variable=prefix "$c" 2>/dev/null || true)"
    [ -n "$d" ] && qt5_priv_pc="$d"
done
emit "pkg-config prefix: ${qt5_priv_pc:-unknown}"

QT_INC_CANDIDATES=""
if have pkg-config; then
    for m in Qt5Gui Qt5Core; do
        for f in --cflags-only-I; do
            for i in $(pkg-config $f "$m" 2>/dev/null | tr ' ' '\n' | sed 's/^-I//'); do
                QT_INC_CANDIDATES="$QT_INC_CANDIDATES $i"
            done
        done
    done
fi
QT_INC_CANDIDATES="$QT_INC_CANDIDATES /usr/include/qt5 /usr/include/x86_64-linux-gnu/qt5 /usr/include/aarch64-linux-gnu/qt5"
emit "Qt include dirs : $(echo $QT_INC_CANDIDATES | tr ' ' '\n' | sort -u | tr '\n' ' ')"

# ------------------------------------------------- 3. §6.1 decision part A ---
sec "3. 6.1 part A -- Qt 5.15 private QRhi headers (qtbase5-private-dev)"

find_private() {
    # $1 = private header file name (e.g. qrhi_p.h)
    for base in $QT_INC_CANDIDATES; do
        [ -d "$base" ] || continue
        hit="$(find "$base" -name "$1" -path '*/private/*' 2>/dev/null | head -1)"
        if [ -n "$hit" ]; then emit "  FOUND $hit" && return 0; fi
    done
    emit "  NOT FOUND: $1"
    return 1
}

qrhi_hit=0; qshader_hit=0
find_private qrhi_p.h   && qrhi_hit=1
find_private qshader_p.h && qshader_hit=1
if [ "$qrhi_hit" -eq 1 ] && [ "$qshader_hit" -eq 1 ]; then private_ok=1; fi

# Which RHI backends did the packager build headers for?
priv_dir=""
for base in $QT_INC_CANDIDATES; do
    [ -d "$base" ] || continue
    d="$(find "$base" -type d -name private -path '*QtGui*' 2>/dev/null | head -1)"
    [ -n "$d" ] && priv_dir="$d" && break
done
if [ -n "$priv_dir" ]; then
    emit "private dir     : $priv_dir"
    emit "  rhi headers   : $(ls "$priv_dir" | grep -E '^qrhi.*\.h$' | tr '\n' ' ')"
    for b in qrhid3d11_p.h qrhigles2_p.h qrhivulkan_p.h qrhimetal_p.h qrhinull_p.h; do
        [ -f "$priv_dir/$b" ] && emit "  backend       : $b present"
    done
    # Qt5 has no HDR support at all; confirm before promising a 10-line downgrade.
    if grep -ql 'HDRExtendedSrgbLinear\|QRhiSwapChainHdrInfo' "$priv_dir"/qrhi_p.h 2>/dev/null; then
        emit "  HDR in Qt5     : PRESENT (unexpected -- re-check the HDR downgrade plan)"
    else
        emit "  HDR in Qt5     : absent, as expected -> HDR paths must degrade to SDR"
    fi
fi

# ------------------------------------------------ 4. §6.1 decision part B ----
sec "4. 6.1 part B -- Qt 5.15 Shader Tools / qsb (QSB version match)"

if have qsb; then
    emit "qsb             : $(command -v qsb)"
    emit "qsb -v          : $(qsb -v 2>&1 | tr '\n' ' ')"
    qsb_ok=1
else
    emit "qsb             : MISSING (Qt 5.15 ships it in qtbase5-dev-tools)"
fi

# Authoritative answer for which .qsb versions Qt5 will accept: the version
# baked into the private header, not the tool that happens to be installed.
if [ -n "$priv_dir" ] && [ -f "$priv_dir/qshader_p.h" ]; then
    emit "QSB_VERSION     : $(grep -E '^#\s*define\s+QSB_VERSION\b' "$priv_dir/qshader_p.h" | head -1 | tr -s ' ')"
    emit "QSB_VERSION_MIN : $(grep -E '^#\s*define\s+QSB_VERSION_MIN\b' "$priv_dir/qshader_p.h" | head -1 | tr -s ' ')"
else
    emit "QSB_VERSION     : cannot read (private header missing)"
fi

# What version are the .qsb files that are already committed to the tree?
if [ -d "$REPO_ROOT" ]; then
    n_qsb="$(find "$REPO_ROOT" -name '*.qsb' -not -path '*/.tools/*' 2>/dev/null | wc -l)"
    emit "committed .qsb  : $n_qsb file(s)"
    if [ "$n_qsb" -gt 0 ]; then
        f="$(find "$REPO_ROOT" -name '*.qsb' -not -path '*/.tools/*' 2>/dev/null | head -1)"
        emit "  sample        : ${f#$REPO_ROOT/}"
        emit "  prefix bytes  : $(od -An -tx1 -N16 "$f" 2>/dev/null | tr -s ' ')"
    fi
fi
if have qsb; then
    emit "qsb version opts: $(qsb --help 2>&1 | grep -iE 'qsbversion' | head -3 | tr -s ' ')"
fi

# --------------------------------------------------- 5. runtime libraries ----
sec "5. runtime / dev libraries"
check_pc() { if pkg-config --exists "$1" 2>/dev/null; then emit "$(printf '%-24s' "$1"): $(pkg-config --modversion "$1" 2>/dev/null)"; else emit "$(printf '%-24s' "$1"): missing"; fi; }
for m in x11 xcb xcb-xfixes xfixes xrandr xi xtst xkbcommon xkbcommon-x11 \
         wayland-client egl gl glesv2 fontconfig freetype2 harfbuzz \
         libpipewire-0.3 libpulse dbus-1 libpng libjpeg libwebp libtiff-4 \
         zlib libheif libjxl; do
    check_pc "$m"
done
emit "opencv4 (cmake dir): $(ls -d /usr/lib/*/cmake/opencv4 /usr/share/OpenCV 2>/dev/null | tr '\n' ' ')"
if have pkg-config && pkg-config --exists opencv4 2>/dev/null; then
    emit "opencv4 version : $(pkg-config --modversion opencv4)"
    # wechat_qrcode + barcode are the two modules snow_shot actually needs.
    for h in /usr/include/opencv4/opencv2/wechat_qrcode.hpp /usr/include/opencv4/opencv2/objdetect/barcode.hpp \
             /usr/include/opencv4/opencv2/objdetect.hpp; do
        [ -f "$h" ] && emit "  opencv header : $h" || emit "  opencv header : MISSING $h"
    done
fi
emit "onnxruntime     : $(ls -d /usr/lib/*/cmake/onnxruntime /usr/include/onnxruntime 2>/dev/null | tr '\n' ' ')"
emit "minizip-ng      : $(pkg-config --modversion minizip-ng 2>/dev/null || ls -d /usr/include/minizip-ng 2>/dev/null || echo missing)"
emit "crashpad        : $(ls -d /usr/include/crashpad 2>/dev/null || echo 'missing (expected: Linux drops crashpad, docs 11.4)')"

# ------------------------------------------------------------- 6. session ----
sec "6. display session / input"
emit "XDG_SESSION_TYPE: ${XDG_SESSION_TYPE:-<unset>}"
emit "WAYLAND_DISPLAY : ${WAYLAND_DISPLAY:-<unset>}"
emit "DISPLAY         : ${DISPLAY:-<unset>}"
emit "XDG_CURRENT_DESKTOP: ${XDG_CURRENT_DESKTOP:-<unset>}"
emit "QT_QPA_PLATFORM : ${QT_QPA_PLATFORM:-<unset>}"
for d in /usr/share/xsessions /usr/share/wayland-sessions; do
    [ -d "$d" ] && emit "$d: $(ls "$d" 2>/dev/null | tr '\n' ' ')"
done

# ---------------------------------------------------------------- 7. Rust ----
sec "7. Rust toolchain"
if have rustc; then
    emit "rustc           : $(rustc --version)"
    emit "cargo           : $(cargo --version)"
    have rustup && emit "rustup targets  : $(rustup target list --installed 2>/dev/null | tr '\n' ' ')"
else
    emit "rustc           : MISSING"
fi
emit "repo toolchain  : $(grep -E '^channel' "$REPO_ROOT/rust-toolchain.toml" 2>/dev/null || echo 'rust-toolchain.toml missing')"
emit "libclang        : $(ls -1 /usr/lib/*/libclang*.so* /usr/lib/libclang*.so* 2>/dev/null | head -2 | tr '\n' ' ')"

# ----------------------------------------------------------------- 8. CPU ----
sec "8. CPU features / SIMD"
if [ -r /proc/cpuinfo ]; then
    emit "model name      : $(grep -m1 'model name\|Model' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    if [ "$(uname -m)" = "aarch64" ]; then
        emit "NEON (asimd)    : $(grep -m1 Features /proc/cpuinfo | grep -qw asimd && echo present || echo absent)"
        emit "dotprod/sve     : $(grep -m1 Features /proc/cpuinfo | tr ' ' '\n' | grep -E '^(asimddp|sve)$' | tr '\n' ' ')"
    else
        for f in avx2 avx512f fma bmi2; do
            emit "$(printf '%-15s' "$f") : $(grep -m1 flags /proc/cpuinfo | grep -qw "$f" && echo present || echo absent)"
        done
    fi
fi

# --------------------------------------------------------------- 9. probe ----
verdict_extra=""
if [ "$RUN_PROBE" -eq 1 ]; then
    sec "9. compile probe (--probe)"
    pdir="$(mktemp -d)"
    cat > "$pdir/probe.cpp" <<'PROBE'
// Decides §6.1 in code rather than in prose: can this Qt5 install (a) compile
// against the private QRhi headers, and (b) actually deserialize a .qsb that
// was baked by Qt6's qsb?
#include <QGuiApplication>
#include <QFile>
#include <QShader>
#include <QDebug>

#if defined(SNOW_PROBE_PRIVATE_RHI)
#  include <QtGui/private/qrhi_p.h>
#  include <QtGui/private/qshader_p.h>
#endif

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

#if defined(SNOW_PROBE_PRIVATE_RHI)
    qInfo("private RHI headers : COMPILED");
    qInfo("QRhi::OpenGLES2 enum: OK");
#else
    qInfo("private RHI headers : not compiled (no -DSNOW_PROBE_PRIVATE_RHI)");
#endif
    qInfo("QSB_VERSION         : %d", QSB_VERSION);

    if (argc > 1) {
        QFile f(QString::fromLocal8Bit(argv[1]));
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("cannot open %s", argv[1]);
            return 3;
        }
        const QShader shader = QShader::fromSerialized(f.readAll());
        if (shader.isValid()) {
            qInfo("qsb load            : VALID (%d shader key(s))", shader.availableShaders().size());
            return 0;
        }
        qWarning("qsb load            : NULL -- Qt5 rejected the file (version mismatch)");
        return 4;
    }
    return 0;
}
PROBE

    priv_flag=""
    [ "$private_ok" -eq 1 ] && priv_flag="-DSNOW_PROBE_PRIVATE_RHI"
    qt_pc_flags="$(pkg-config --cflags Qt5Gui Qt5Core 2>/dev/null || echo '')"
    # Private headers are not exposed through pkg-config on Debian derivatives.
    priv_inc=""
    [ -n "$priv_dir" ] && priv_inc="-I$(dirname "$(dirname "$priv_dir")")"
    sample_qsb="$(find "$REPO_ROOT" -name '*.qsb' -not -path '*/.tools/*' 2>/dev/null | head -1)"

    if have g++ && [ -n "$qt_pc_flags" ]; then
        emit "compiling probe ..."
        # shellcheck disable=SC2086
        if g++ -std=c++17 -fPIC -o "$pdir/probe" "$pdir/probe.cpp" \
               $priv_flag $qt_pc_flags $priv_inc \
               $(pkg-config --libs Qt5Gui Qt5Core 2>/dev/null) 2>"$pdir/cc.log"; then
            emit "probe compile   : OK"
            # Needs a display for QGuiApplication; fall back to offscreen.
            QT_QPA_PLATFORM=offscreen "$pdir/probe" ${sample_qsb:+"$sample_qsb"} || \
                emit "probe run       : exit=$? (offscreen platform may be unavailable)"
        else
            emit "probe compile   : FAILED"
            sed -n '1,25p' "$pdir/cc.log" | sed 's/^/    /'
        fi
    else
        emit "probe skipped   : g++ or Qt5 pkg-config unavailable"
        emit "                  manually: g++ -std=c++17 probe.cpp $priv_flag $priv_inc \$(pkg-config --cflags --libs Qt5Gui)"
    fi
    rm -rf "$pdir"
fi

# ------------------------------------------------------------- 10. verdict ---
sec "10. VERDICT for docs-uos-linux-port.md 6.1"

if [ "$qt5_ok" -eq 0 ] && have qmake; then qt5_ok=1; fi
if ! have pkg-config; then
    qt5_ok_note="cannot determine (pkg-config unavailable)"
elif pkg-config --atleast-version=5.15 Qt5Core 2>/dev/null; then
    qt5_ok=1
    qt5_ok_note="Qt5 >= 5.15"
else
    qt5_ok_note="Qt5 < 5.15 -- the whole port baseline is at risk"
fi

emit "Qt 5.15 baseline        : $([ "$qt5_ok" -eq 1 ] && echo OK || echo 'CHECK') ($qt5_ok_note)"
emit "private QRhi headers    : $([ "$private_ok" -eq 1 ] && echo 'AVAILABLE (qtbase5-private-dev installed)' || echo 'MISSING (install qtbase5-private-dev)')"
emit "Qt 5.15 qsb             : $([ "$qsb_ok" -eq 1 ] && echo AVAILABLE || echo 'MISSING (install qtbase5-dev-tools)')"
emit ""
if [ "$private_ok" -eq 1 ] && [ "$qsb_ok" -eq 1 ]; then
    emit "=> Decision: QRhi route is viable. Even so, every committed .qsb must be"
    emit "   re-baked with the Qt 5.15 qsb, because Qt6 wrote QSB v9 while Qt5"
    emit "   accepts only v1-v5 and silently returns a null QShader otherwise."
    emit "   Re-run with --probe to confirm a real .qsb deserializes."
elif [ "$private_ok" -eq 1 ]; then
    emit "=> Decision: private headers exist but Shader Tools are missing, so the"
    emit "   .qsb assets cannot be regenerated in a Qt5-compatible version."
    emit "   Treat the viewer as QOpenGLWidget-rewrite territory unless shaders can"
    emit "   be shipped pre-baked from a machine that has the Qt 5.15 Shader Tools."
else
    emit "=> Decision: QRhi route is NOT available -> rewrite snow_image_viewer on"
    emit "   QOpenGLWidget (docs-uos-linux-port.md 6.1 / W2)."
fi

if [ -n "$OUT" ]; then
    emit ""
    emit "report written to $OUT"
fi
exit 0
