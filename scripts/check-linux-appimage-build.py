#!/usr/bin/env python3
"""Validate a built AppImage the way a machine would have to run it.

`appimagetool` is the only step of the Linux packaging path that has never
executed in this project: the Windows workstation has no appimagetool, so
every `COMMAND` in `cmake/SnowShotAppImage.cmake` was read but never run. This
script is what turns "the CMake code looks right" into "the file that came out
is a type-2 AppImage for the right architecture, and unpacking it yields the
entry, the icon and the binary".

The checks are deliberately about the *product*, not about the tooling:

  * the file is an ELF whose e_machine is the architecture we asked for -- a
    mismatch here means the type-2 runtime that got embedded was for another
    machine, and the AppImage would fail with "Exec format error" on the
    target rather than at build time;
  * bytes 8..10 are `AI\\x02`, the type-2 AppImage magic;
  * unpacking it (`--appimage-extract`, which needs no FUSE) yields the
    AppRun, the desktop entry, the icon the entry names, the executable and
    the bundled shared libraries;
  * the AppRun inside the package is byte-identical to the reviewed one in
    the repository -- a packaging step that rewrites the entry would
    otherwise slip through.

Usage:
    python scripts/check-linux-appimage-build.py --appimage <file> [--expect-arch x86_64|aarch64]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# ELF e_machine values for the two architectures this project packages.
MACHINES = {"x86_64": 0x3E, "aarch64": 0xB7}

# The type-2 AppImage magic sits right after the ELF identification bytes.
APPIMAGE_MAGIC = b"AI\x02"

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def parse_desktop(path: Path) -> dict[str, str]:
    """Parse the desktop entry's key=value pairs (no configparser: no locales)."""
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appimage", required=True, help="the AppImage to validate")
    parser.add_argument("--expect-arch", choices=sorted(MACHINES),
                        help="the architecture the AppImage must be built for")
    args = parser.parse_args()

    appimage = Path(args.appimage)
    if not appimage.is_file():
        check("the AppImage exists", False, f"missing: {appimage}")
        return 1
    check("the AppImage exists", True)

    size = appimage.stat().st_size
    check("the AppImage is not empty", size > 1024 * 1024, f"{size} bytes")

    head = appimage.read_bytes()[:20]
    check("the AppImage is an ELF file", head[:4] == b"\x7fELF", repr(head[:4]))
    check("the AppImage carries the type-2 magic", head[8:11] == APPIMAGE_MAGIC,
          repr(head[8:11]))
    machine = int.from_bytes(head[18:20], "little")
    if args.expect_arch:
        want = MACHINES[args.expect_arch]
        check(f"the AppImage is built for {args.expect_arch}", machine == want,
              f"e_machine 0x{machine:x}, expected 0x{want:x}")

    # Unpack it the way a machine without FUSE would.
    workdir = Path(tempfile.mkdtemp(prefix="snow-appimage-check-"))
    try:
        unpacked = workdir / "squashfs-root"
        result = subprocess.run(
            [str(appimage), "--appimage-extract"],
            cwd=workdir, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            # A failure here is the script's own problem, not the package's;
            # say so instead of reporting every layout check as missing.
            check("the AppImage unpacks", False,
                  f"exit {result.returncode}: {result.stderr.strip()[:400]}")
            return 1
        check("the AppImage unpacks", True)

        app_run = unpacked / "AppRun"
        check("the package has an AppRun", app_run.is_file())
        check("the AppRun is executable", app_run.is_file() and os.access(app_run, os.X_OK))

        desktop = unpacked / "snow-shot.desktop"
        check("the package has a desktop entry at its root", desktop.is_file())
        if desktop.is_file():
            values = parse_desktop(desktop)
            icon = values.get("Icon", "")
            check("the desktop entry names an icon", bool(icon), "no Icon= key")
            check("the named icon is at the root of the package",
                  bool(icon) and (unpacked / icon).is_file(),
                  f"Icon={icon}")
            check("the desktop entry has a type", values.get("Type") == "Application",
                  f"Type={values.get('Type')!r}")
            check("the desktop entry has an exec line", bool(values.get("Exec")))

        check("the package has the executable",
              (unpacked / "usr" / "bin" / "snow_shot").is_file())
        # `GNUInstallDirs` decides the library directory at configure time, and
        # on a Debian-style host it is `lib/<multiarch>` rather than `lib`. The
        # AppDir installs under `<appdir>/usr`, which is not the literal `/usr`
        # the multiarch rule keys on, so both layouts are possible -- search the
        # whole `usr` tree instead of guessing one.
        usr = unpacked / "usr"
        libs = sorted(usr.rglob("*.so*")) if usr.is_dir() else []
        if libs:
            listing = ""
        elif usr.is_dir():
            files = sorted(str(p.relative_to(unpacked)) for p in usr.rglob("*") if p.is_file())
            listing = "no *.so* under usr/: " + ", ".join(files)[:400]
        else:
            listing = "usr/ is missing"
        check("the package bundles shared libraries", bool(libs), listing)

        # The entry that ends up in the package must be the reviewed one. The
        # file lives at the repository root (`packaging/linux/appimage/AppRun`,
        # the path `cmake/SnowShotAppImage.cmake` copies from), not under
        # `snow_shot/`: looking in the wrong place made this check silently
        # skip, which is worse than not having it.
        reviewed = REPO / "packaging" / "linux" / "appimage" / "AppRun"
        check("the reviewed AppRun exists in the repository", reviewed.is_file(), str(reviewed))
        if app_run.is_file() and reviewed.is_file():
            check("the packaged AppRun is the reviewed one",
                  app_run.read_bytes() == reviewed.read_bytes(),
                  "the packaged AppRun differs from packaging/linux/appimage/AppRun")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nAppImage build check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
