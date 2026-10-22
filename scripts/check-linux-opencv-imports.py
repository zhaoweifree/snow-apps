#!/usr/bin/env python3
"""The consumer side of the static OpenCV: imported targets it must define.

`OpenCVConfig.cmake` (OpenCV 4.12) contains **no** `find_dependency` call. Every
third-party imported target that a module's exported link interface names has
to exist already, or `include(OpenCVModules.cmake)` aborts:

    CMake Error at .../share/opencv4/OpenCVModules.cmake:112 (set_target_properties):
      The link interface of target "opencv_wechat_qrcode" contains: Iconv::Iconv
      but the target was not found.                       (CI run 22)

That is invisible on Windows and macOS, where OpenCV comes from a package
manager whose config defines these targets itself -- the Linux static build is
the only one that exports an `Iconv::Iconv` entry, so only Linux can break.
This gate keeps the defining `find_package` where it works: in the root
`CMakeLists.txt`, before the subdirectories that call `find_package(OpenCV)`.

Run it directly; the Linux packaging gate runs it as well.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROOT_CMAKE = ROOT / "CMakeLists.txt"

# Every imported target the Linux static OpenCV export is known to reference.
# One entry per target, with the module that pulls it in, so a new failure of
# this class is a one-line addition rather than a re-investigation.
REQUIRED_TARGETS = {
    "Iconv::Iconv": "opencv_wechat_qrcode (ICE: see run 22)",
}

CONSUMER_PATTERN = re.compile(r"find_package\s*\(\s*OpenCV\b")
ADD_SUBDIR_PATTERN = re.compile(r"add_subdirectory\s*\(\s*([A-Za-z0-9_.\-]+)")

_failures = 0


def fail(message: str) -> None:
    global _failures
    print(f"FAIL: {message}")
    _failures += 1


def opencv_consumer_dirs() -> list[str]:
    """Directories whose CMakeLists.txt looks OpenCV up."""
    dirs = []
    for path in sorted(ROOT.glob("*/CMakeLists.txt")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if CONSUMER_PATTERN.search(text):
            dirs.append(path.parent.name)
    return dirs


def main() -> int:
    if not ROOT_CMAKE.is_file():
        print(f"FAIL: {ROOT_CMAKE} is missing")
        return 1

    lines = ROOT_CMAKE.read_text(encoding="utf-8").splitlines()

    consumers = opencv_consumer_dirs()
    if not consumers:
        fail("no directory calls find_package(OpenCV) -- this gate has lost its subject")
        return 1

    for target, reason in REQUIRED_TARGETS.items():
        package = target.split("::")[0]
        index = next(
            (i for i, line in enumerate(lines)
             if re.search(rf"find_package\s*\(\s*{package}\b", line)),
            None,
        )
        if index is None:
            fail(
                f"the root CMakeLists.txt never calls find_package({package}), but the "
                f"installed OpenCV exports {target} ({reason}); every "
                "find_package(OpenCV CONFIG) would abort"
            )
            continue

        # It has to come before the subdirectory that consumes OpenCV: an
        # imported target is only visible to directories added afterwards.
        for consumer in consumers:
            subdir = next(
                (i for i, line in enumerate(lines)
                 if (m := ADD_SUBDIR_PATTERN.search(line)) and m.group(1) == consumer),
                None,
            )
            if subdir is None:
                fail(
                    f"{consumer} calls find_package(OpenCV) but the root "
                    f"CMakeLists.txt never adds it with add_subdirectory"
                )
                continue
            if index > subdir:
                fail(
                    f"find_package({package}) is on line {index + 1}, after "
                    f"add_subdirectory({consumer}) on line {subdir + 1}: {target} "
                    f"would not exist yet when {consumer} looks OpenCV up"
                )

        # The lookup is only safe on the platform whose OpenCV needs it; on
        # Windows and macOS the package manager's config defines the target and
        # a stray find_package could pick up a different iconv.
        guard = "\n".join(lines[max(0, index - 30): index])
        if not re.search(r"if\s*\(\s*UNIX\b", guard):
            fail(
                f"find_package({package}) is not guarded by an if(UNIX ...) block; "
                "the Linux static OpenCV is the only build that needs it"
            )

    if _failures:
        print(f"{_failures} OpenCV import check(s) failed")
        return 1
    print(
        f"OpenCV import check passed ({len(REQUIRED_TARGETS)} imported target(s), "
        f"{len(consumers)} consumer(s): {', '.join(consumers)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
