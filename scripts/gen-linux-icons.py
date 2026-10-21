#!/usr/bin/env python3
"""Materialise the hicolor PNG icons from the committed Windows icon.

Why the ``.ico`` is the source
------------------------------
The Linux package needs fixed-size PNGs, not just the scalable SVG, because
XFCE panel, dde-dock and several AppImage launchers resolve ``Icon=snow-shot``
through the hicolor lookup only and never fall back to ``scalable/``.
Rasterising the SVG at configure time would mean depending on
``rsvg-convert``/ImageMagick, which a distribution build root does not
guarantee; and it would make the shipped icon depend on whichever renderer
happened to be installed. ``app-icon.ico`` already carries the artist's
exported bitmaps for every size we need, each one a complete PNG stream inside
the icon container, so the conversion is a pure byte copy with no rendering at
all.

Sizes
-----
Only the sizes freedesktop.org lists for hicolor are installed. The icon also
contains 20x20 and 40x40, which no desktop environment looks up by directory
name, so those are dropped rather than installed into non-standard paths.
512x512 is absent from the source and is covered by the scalable SVG that the
install rule ships alongside.

Usage
-----
``python scripts/gen-linux-icons.py``          rewrite the PNG tree
``python scripts/gen-linux-icons.py --check``  verify it is still in sync

``--check`` is what the packaging gate runs: it compares the working tree
against the icon byte for byte, so editing ``app-icon.ico`` without rerunning
the generator fails the build instead of shipping one stale size.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ICO = REPO / "snow_shot" / "resources" / "app-icon.ico"
ICON_ROOT = REPO / "snow_shot" / "resources" / "icons" / "hicolor"
ICON_NAME = "snow-shot.png"

# The hicolor directory names freedesktop.org defines for application icons.
HICOLOR_SIZES = (16, 24, 32, 48, 64, 128, 256)

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_ico(path: Path) -> dict[int, bytes]:
    """Return ``{size: png bytes}`` for every PNG entry in the icon.

    Only PNG-compressed entries are returned. A BMP entry (the old 32bpp DIB
    form) would need a full decoder and a PNG encoder to become a file we can
    install, and the current source has none -- bail out loudly instead of
    silently producing a shorter list.
    """
    data = path.read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1:
        raise SystemExit(f"{path}: not an icon container (reserved={reserved} type={kind})")
    entries: dict[int, bytes] = {}
    for index in range(count):
        width, height, _colours, _reserved, _planes, _bpp, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + index * 16
        )
        # A stored 0 means 256: the field is a byte and 256 does not fit.
        width = width or 256
        height = height or 256
        if width != height:
            raise SystemExit(f"{path}: entry {index} is {width}x{height}, expected a square")
        blob = data[offset : offset + size]
        if not blob.startswith(PNG_SIGNATURE):
            raise SystemExit(
                f"{path}: entry {index} ({width}x{height}) is not PNG-compressed; "
                "regenerate app-icon.ico with PNG compression enabled"
            )
        entries[width] = blob
    return entries


def png_size(blob: bytes) -> tuple[int, int]:
    """Read width and height out of the IHDR chunk."""
    if not blob.startswith(PNG_SIGNATURE) or blob[12:16] != b"IHDR":
        raise ValueError("not a PNG with a leading IHDR chunk")
    return struct.unpack_from(">II", blob, 16)


def expected_files() -> dict[Path, bytes]:
    entries = read_ico(ICO)
    missing = [size for size in HICOLOR_SIZES if size not in entries]
    if missing:
        raise SystemExit(f"{ICO.name} has no PNG entry for: {', '.join(map(str, missing))}")
    planned: dict[Path, bytes] = {}
    for size in HICOLOR_SIZES:
        blob = entries[size]
        got = png_size(blob)
        if got != (size, size):
            raise SystemExit(
                f"{ICO.name}: entry labelled {size} is actually {got[0]}x{got[1]}"
            )
        planned[ICON_ROOT / f"{size}x{size}" / "apps" / ICON_NAME] = blob
    return planned


def write(planned: dict[Path, bytes]) -> int:
    for path, blob in planned.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        print(f"wrote {path.relative_to(REPO)} ({len(blob)} bytes)")
    return 0


def check(planned: dict[Path, bytes]) -> int:
    problems: list[str] = []
    for path, blob in planned.items():
        if not path.exists():
            problems.append(f"missing {path.relative_to(REPO)}")
            continue
        actual = path.read_bytes()
        if actual != blob:
            problems.append(f"out of sync {path.relative_to(REPO)}")
            continue
        width, height = png_size(actual)
        directory = path.parent.parent.name
        if directory != f"{width}x{height}":
            problems.append(
                f"{path.relative_to(REPO)} is {width}x{height} but sits in {directory}"
            )
    for nested in sorted(ICON_ROOT.glob("*/*/" + ICON_NAME)):
        if nested not in planned:
            problems.append(f"unexpected {nested.relative_to(REPO)} (not a hicolor size)")
    if problems:
        for problem in problems:
            print(f"FAIL {problem}")
        print("\nRun: python scripts/gen-linux-icons.py")
        return 1
    for path in planned:
        print(f"ok   {path.relative_to(REPO)}")
    print(f"\n{len(planned)} hicolor icon(s) in sync with {ICO.name}")
    return 0


def main(argv: list[str]) -> int:
    planned = expected_files()
    if "--check" in argv:
        return check(planned)
    return write(planned)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
