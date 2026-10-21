#!/usr/bin/env python3
"""Keep `${SNOW_QT_NS}::<Module>Private` link lines paired with their resolver.

Background
----------
`snow_find_qt_private(<Module>)` is the only place that makes a Qt5 private
module usable: on a distribution kit it creates the `Qt5::<Module>Private`
imported target from the kit's private headers, and on a kit that ships the
target it applies the include directories to the current directory
(`cmake/SnowQt.cmake`). A `target_link_libraries(... ${SNOW_QT_NS}::GuiPrivate)`
line in a CMakeLists that never calls the resolver therefore fails at configure
time with "Target links to Qt5::GuiPrivate", naming the target but not the
missing call (CI run 39600792545).

This check is textual for the same reason as
`check-cmake-list-dir-scope.py`: nothing local configures the Qt5 route of
every subproject, so the mistake would otherwise wait for a real Linux build.

Rule: for every `CMakeLists.txt` that mentions `${SNOW_QT_NS}::<Module>Private`
(or a literal `Qt5::`/`Qt6::` equivalent), the same file must call
`snow_find_qt_private(<Module>)`.

Usage: python scripts/check-qt-private-modules.py [--root .]
Exit: 0 clean, 1 violations found.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LINK = re.compile(r"\$\{SNOW_QT_NS\}::(\w+)Private|(?:Qt5|Qt6)::(\w+)Private")
RESOLVE = re.compile(r"snow_find_qt_private\s*\(\s*(\w+)\s*\)")

# Directories that hold generated or third-party copies of the tree.
SKIP_DIRS = {"build", ".git", ".workbuddy", ".tools", "node_modules", "target"}


def iter_cmakelists(root: Path):
    for path in sorted(root.rglob("CMakeLists.txt")):
        parts = set(path.relative_to(root).parts[:-1])
        if parts & SKIP_DIRS:
            continue
        yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    problems: list[str] = []
    examined = 0

    for path in iter_cmakelists(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "Private" not in text:
            continue
        examined += 1
        linked = {a or b for a, b in LINK.findall(text)}
        if not linked:
            continue
        resolved = set(RESOLVE.findall(text))
        for module in sorted(linked - resolved):
            # Report the line of the first offending link for a readable message.
            line = next(
                (n for n, raw in enumerate(text.splitlines(), 1) if f"{module}Private" in raw),
                0,
            )
            problems.append(
                f"{path.relative_to(root).as_posix()}:{line}: links "
                f"${{SNOW_QT_NS}}::{module}Private but never calls "
                f"snow_find_qt_private({module})"
            )

    for problem in problems:
        print(problem)

    print(
        f"checked {examined} CMakeLists.txt that mention a private module: "
        f"{'OK' if not problems else f'{len(problems)} problem(s)'}"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
