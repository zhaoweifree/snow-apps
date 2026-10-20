#!/usr/bin/env python3
"""Install the audited Qt 6.12.0 MSVC 2022 x64 kit without the Qt Online Installer.

Why this exists
---------------
The project pins Qt to an exact version (scripts/qt-toolchain.json ->
qtVersion 6.12.0, consumed by cmake/SnowQt.cmake with find_package(Qt6
"... " EXACT)), so the baseline check has to run against exactly 6.12.0.

Neither of the usual installers works here:

* aqtinstall 3.3.0 (the only version reachable in this environment) still
  assumes the pre-6.12 repository layout
  ``online/qtsdkrepository/windows_x86/desktop/qt6_6120/Updates.xml``.
  Qt 6.12 publishes one directory per architecture instead, i.e.
  ``.../desktop/qt6_6120/qt6_6120_msvc2022_64/Updates.xml``, so aqt fails
  with "Failed to download checksum for the file 'Updates.xml'".
* the Qt Online Installer needs an interactive Qt account.

So this script speaks the repository protocol directly: read Updates.xml,
download the handful of archives we need, verify the published .sha1,
extract them into the kit prefix.

Layout produced
---------------
    <prefix>/bin/qmake.exe, <prefix>/lib/cmake/Qt6/Qt6Config.cmake, ...
with <prefix> defaulting to C:/Qt/6.12.0/msvc2022_64 (mirrors the existing
C:/Qt/5.15.2/msvc2019_64 kit layout).

Usage
-----
    python install-qt6.py                # download + extract (idempotent)
    python install-qt6.py --check        # verify an existing prefix only
    python install-qt6.py --mirror https://download.qt.io
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

QT_VERSION = "6.12.0"
QT_VERSION_TAG = "6120"
ARCH = "win64_msvc2022_64"
# Repository package folder for the whole desktop kit. The per-archive file
# names below are prefixed with the package Version string (release stamp),
# which is read from Updates.xml rather than hardcoded.
PACKAGE = f"qt.qt6.{QT_VERSION_TAG}.{ARCH}"
HOST_PATH = "windows_x86/desktop"
SUBDIR = f"qt6_{QT_VERSION_TAG}/qt6_{QT_VERSION_TAG}_msvc2022_64"

DEFAULT_MIRROR = "https://mirrors.tuna.tsinghua.edu.cn/qt"
FALLBACK_MIRROR = "https://download.qt.io"

# What the project actually links against (see the snow_find_qt() call sites):
#   qtbase          -> Core Gui Widgets Network Test Concurrent PrintSupport
#                      OpenGL OpenGLWidgets + private headers (Qt6*Private)
#   qtsvg           -> ant_design_qt, snow_shot icon pipeline
#   qttools         -> LinguistTools (lrelease/lupdate, required by qt_add_translation)
#   qttranslations  -> qt_zh_CN.qm etc., needed again at packaging time (P7)
# qtdeclarative/qtdoc/d3dcompiler/opengl32sw are deliberately skipped: no QML
# in the product code, and the software-GL fallback belongs to the packaging step.
WANTED = ["qtbase", "qtsvg", "qttools", "qttranslations"]

DEFAULT_PREFIX = Path("C:/Qt") / QT_VERSION / "msvc2022_64"
# scripts/ -> repo root, then the shared tool cache next to vcpkg.
DEFAULT_CACHE = Path(__file__).resolve().parents[1] / ".tools" / "qt6-download"


def log(msg: str) -> None:
    print(f"[qt6] {msg}", flush=True)


def http_get(url: str, dest: Path, retries: int = 5) -> None:
    """Stream url into dest with resume support and a few retries."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(1, retries + 1):
        have = dest.stat().st_size if dest.exists() else 0
        req = urllib.request.Request(url, headers={"User-Agent": "snow-qt6-installer/1"})
        if have:
            req.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                total = resp.headers.get("Content-Length")
                total = have + int(total) if total else None
                if resp.status == 200:
                    have = 0  # server ignored Range, restart cleanly
                    mode = "wb"
                else:
                    mode = "ab"
                with dest.open(mode) as fh:
                    chunk = 1 << 20
                    done = have
                    last = time.time()
                    while True:
                        buf = resp.read(chunk)
                        if not buf:
                            break
                        fh.write(buf)
                        done += len(buf)
                        if time.time() - last > 5:
                            last = time.time()
                            pct = f" ({done * 100 // total}%)" if total else ""
                            log(f"  {dest.name}: {done/1048576:.1f} MiB{pct}")
            return
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            if attempt == retries:
                raise
            log(f"  retry {attempt}/{retries} after {exc!r}")
            time.sleep(2 * attempt)


def sha1_of(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch_updates_xml(mirror: str, cache: Path) -> ET.Element:
    url = f"{mirror}/online/qtsdkrepository/{HOST_PATH}/{SUBDIR}/Updates.xml"
    dest = cache / "Updates.xml"
    log(f"Updates.xml <- {url}")
    http_get(url, dest)
    return ET.parse(dest).getroot()


def package_archives(root: ET.Element) -> tuple[str, dict[str, str]]:
    """Return (release_stamp, {archive_basename_without_ext: sha1_prefix})."""
    for pu in root.findall("PackageUpdate"):
        if pu.findtext("Name") != PACKAGE:
            continue
        version = pu.findtext("Version") or ""
        stamp = version.split("-")[0] + "-" + version.split("-")[1] + "-" + version.split("-")[2]
        archives = (pu.findtext("DownloadableArchives") or "").strip()
        return stamp, {a.strip().removesuffix(".7z"): "" for a in archives.split(",") if a.strip()}
    raise SystemExit(f"[qt6] package {PACKAGE} not found in Updates.xml")


def extract(archive: Path, prefix: Path) -> None:
    import py7zr

    log(f"extract {archive.name} -> {prefix}")
    prefix.mkdir(parents=True, exist_ok=True)
    with py7zr.SevenZipFile(archive, "r") as z:
        z.extractall(path=str(prefix))


def verify(prefix: Path) -> list[str]:
    problems: list[str] = []
    checks = {
        "Qt6Config.cmake": prefix / "lib/cmake/Qt6/Qt6Config.cmake",
        "qmake": prefix / "bin/qmake.exe",
        "QtWidgets private headers": prefix / "include/QtWidgets" / QT_VERSION / "QtWidgets/private",
        "QtCore private headers": prefix / "include/QtCore" / QT_VERSION / "QtCore/private",
        "QtSvg config": prefix / "lib/cmake/Qt6Svg/Qt6SvgConfig.cmake",
        "LinguistTools config": prefix / "lib/cmake/Qt6LinguistTools/Qt6LinguistToolsConfig.cmake",
        "lrelease": prefix / "bin/lrelease.exe",
    }
    for label, path in checks.items():
        if not path.exists():
            problems.append(f"missing {label}: {path}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", type=Path, default=DEFAULT_PREFIX)
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    ap.add_argument("--mirror", default=DEFAULT_MIRROR)
    ap.add_argument("--check", action="store_true", help="only verify an existing prefix")
    args = ap.parse_args()

    prefix: Path = args.prefix
    cache: Path = args.cache

    if args.check:
        problems = verify(prefix)
        for p in problems:
            log(f"FAIL {p}")
        log("prefix OK" if not problems else f"{len(problems)} problem(s)")
        return 1 if problems else 0

    root = fetch_updates_xml(args.mirror, cache)
    stamp, archives = package_archives(root)

    selected = [n for n in archives if any(w in n for w in WANTED)]
    if len(selected) != len(WANTED):
        raise SystemExit(f"[qt6] expected archives for {WANTED}, got {selected}")

    pkg_url = f"{args.mirror}/online/qtsdkrepository/{HOST_PATH}/{SUBDIR}/{PACKAGE}"
    for name in selected:
        remote = f"{stamp}{name}.7z"
        local = cache / remote
        if local.exists():
            log(f"cached {remote} ({local.stat().st_size/1048576:.1f} MiB)")
        else:
            log(f"download {remote}")
            http_get(f"{pkg_url}/{remote}", local)
        sha1_file = cache / f"{remote}.sha1"
        if not sha1_file.exists():
            http_get(f"{pkg_url}/{remote}.sha1", sha1_file)
        expected = sha1_file.read_text(encoding="utf-8").split()[0].strip().lower()
        actual = sha1_of(local)
        if expected and actual != expected:
            raise SystemExit(f"[qt6] sha1 mismatch for {remote}: {actual} != {expected}")
        log(f"  sha1 ok {actual[:12]}")

    for name in selected:
        extract(cache / f"{stamp}{name}.7z", prefix)

    problems = verify(prefix)
    for p in problems:
        log(f"FAIL {p}")
    if problems:
        return 1
    log(f"Qt {QT_VERSION} {ARCH} ready at {prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
