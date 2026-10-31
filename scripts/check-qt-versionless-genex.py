#!/usr/bin/env python3
"""A generator expression that needs a *file* needs a concrete Qt target.

Qt 5.15 provides the versionless targets (`Qt::Core`) so that a project written
against Qt 6 keeps working, but they are INTERFACE wrappers around `Qt5::Core`,
not the library itself. Linking through them is fine; asking for their file is
not, and CMake says so at generate time:

    CMake Error at snow_shot/CMakeLists.txt:11302 (add_test):
      Error evaluating generator expression:
        $<TARGET_FILE_DIR:Qt::Core>
      Target "Qt::Core" is not an executable or library.

That is how `Unit tests` died in run 46, on both architectures: 375 `add_test`
calls prepend the Qt library directory to the test's `PATH` (the Windows need to
find `Qt6Core.dll`), and every one of them named the versionless target.

The fix is `${SNOW_QT_NS}::Core` -- the namespace the Qt resolution layer already
publishes, and the spelling `cmake/SnowQtOffscreenTest.cmake` has used all along.
The same expression written with a literal version is the mirror image of that
mistake: `$<TARGET_FILE_DIR:Qt6::Core>` is correct on the Qt 6 route and
unresolvable on the Qt 5 one, where no `Qt6::` target exists at all. It cost a
configure in the `windows-qt5-tests` rung (the expression sits in `if(WIN32)`
inside the test block, so nothing on the Qt 5 route had ever evaluated it).
Both spellings are therefore rejected: a file expression never names a Qt
version literally, and never names a versionless target either. What it names is
`${SNOW_QT_NS}::<name>`, on both routes, always.

This gate keeps the three halves honest: no versionless target in a file
expression, no literal `Qt5::`/`Qt6::` in one either, and `SNOW_QT_NS` really
assigned for both routes (an empty substitution would fail just as loudly, one
layer further away).

Usage: `python scripts/check-qt-versionless-genex.py` (the Linux packaging gate
runs it as well).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

SKIP_DIRS = {".git", ".tools", "build", "node_modules", "__pycache__"}

# Every generator expression whose argument is resolved to a file on disk. The
# list is CMake's, not a guess: these are the ones that require the named target
# to be an executable or a library.
FILE_EXPRESSIONS = (
    "TARGET_FILE",
    "TARGET_FILE_DIR",
    "TARGET_FILE_NAME",
    "TARGET_LINKER_FILE",
    "TARGET_LINKER_FILE_DIR",
    "TARGET_LINKER_FILE_NAME",
    "TARGET_SONAME_FILE",
    "TARGET_SONAME_FILE_DIR",
    "TARGET_SONAME_FILE_NAME",
    "TARGET_PDB_FILE",
    "TARGET_PDB_FILE_DIR",
    "TARGET_PDB_FILE_NAME",
)

VERSIONLESS = re.compile(
    r"\$<\s*(?:" + "|".join(FILE_EXPRESSIONS) + r"):\s*(Qt::[A-Za-z0-9_]+)"
)

# A literal `Qt5::`/`Qt6::` inside a file expression hard-codes one route. The
# namespace variable is the only spelling that satisfies both.
PINNED = re.compile(
    r"\$<\s*(?:" + "|".join(FILE_EXPRESSIONS) + r"):\s*(Qt[56]::[A-Za-z0-9_]+)"
)

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def cmake_files():
    for path in sorted(REPO.rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(REPO).parts[:-1]):
            continue
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake" or path.suffix == ".cmake.in":
            yield path


def genex_targets_in_file(path: Path):
    """Yield (line number, matched text) for versionless Qt file expressions."""
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), start=1):
        # A comment explains the rule somewhere; only code counts.
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        for match in VERSIONLESS.finditer(line):
            yield number, match.group(0), match.group(1)
        for match in PINNED.finditer(line):
            yield number, match.group(0), match.group(1)


def main() -> int:
    offenders = 0
    scanned = 0
    for path in cmake_files():
        scanned += 1
        for number, expression, target in genex_targets_in_file(path):
            offenders += 1
            relative = path.relative_to(REPO)
            print(f"FAIL {relative}:{number}: {expression}")
            if target.startswith("Qt::"):
                print(f"     {target} is an INTERFACE target on the Qt 5 route; "
                      f"use ${{SNOW_QT_NS}}::{target.split('::', 1)[1]} instead")
            else:
                print(f"     {target} only exists on one route; "
                      f"use ${{SNOW_QT_NS}}::{target.split('::', 1)[1]} instead")
    check(f"no file generator expression hard-codes or versionless-names a Qt "
          f"target ({scanned} CMake file(s) scanned)", offenders == 0,
          f"{offenders} occurrence(s)")

    # The substitution above is only safe while the namespace is assigned on
    # both routes; `${SNOW_QT_NS}` expanding to nothing produces
    # `$<TARGET_FILE_DIR::Core>` and the same class of generate-time error.
    resolution = REPO / "cmake" / "SnowQt.cmake"
    text = resolution.read_text(encoding="utf-8", errors="replace") if resolution.exists() else ""
    check("cmake/SnowQt.cmake exists", resolution.exists(), str(resolution))
    for namespace, route in (("Qt6", "6"), ("Qt5", "5")):
        check(f"the Qt {route} route sets SNOW_QT_NS to {namespace}",
              re.search(rf'set\(\s*SNOW_QT_NS\s+"?{namespace}"?\s*\)', text) is not None,
              "every file generator expression in the test registrations depends on it")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nQt versionless generator expression check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
