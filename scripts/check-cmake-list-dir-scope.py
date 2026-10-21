#!/usr/bin/env python3
"""Reject `CMAKE_CURRENT_LIST_DIR` used inside a `function()` or `macro()` body.

Why this exists
---------------
`CMAKE_CURRENT_LIST_DIR` is **dynamic**: it reports the directory of the listfile
*currently being processed*, which for a function body is the **call site**, not
the file that defined the function. `cmake/SnowQt.cmake:26` already documents the
trap for macros and freezes its own directory in file scope; the Qt5 route then
repeated the mistake twice more:

  * `cmake/SnowQtVersionShims.cmake` -> `_snow_qt_shim_bin_dir()` included
    `SnowQt5InstallPaths.cmake` "next to me" and instead looked in
    `ant_design_qt/`, because that is who calls the function.
  * `cmake/SnowShotTranslationRelease.cmake` -> same shape, resolved against
    `snow_shot/`.

Both were found by CI (run 39599176029), never locally: the local fixture
`scripts/check-qt5-install-paths.cmake` runs those resolvers at *file* scope, so
the call-site semantics never appear. A `cmake -P` gate structurally cannot
reproduce it, which is exactly why this check is textual.

The rule this enforces: a module that needs its own directory must evaluate it
once at file scope and reference the variable from inside functions, or use
`CMAKE_CURRENT_FUNCTION_LIST_DIR` (CMake >= 3.17), which is explicitly scoped to
the defining file.

Scope: every `*.cmake` under `cmake/` (recursively). Those are the files included
from many different directories, so they are the ones where the call site varies.

Usage: python scripts/check-cmake-list-dir-scope.py [--root .]
Exit: 0 clean, 1 violations found.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Whole-word match; `CMAKE_CURRENT_FUNCTION_LIST_DIR` does not contain this
# substring, so the correct replacement is never flagged.
TRAP = re.compile(r"\bCMAKE_CURRENT_(?:LIST_DIR|LIST_FILE)\b")
OPENS = re.compile(r"^\s*(function|macro)\s*\(")
CLOSES = re.compile(r"^\s*end(function|macro)\b")


def strip_comment(line: str) -> str:
    """Drop a trailing `#` comment, respecting double-quoted strings."""
    out = []
    in_quote = False
    escaped = False
    for char in line:
        if escaped:
            out.append(char)
            escaped = False
            continue
        if char == "\\":
            out.append(char)
            escaped = True
            continue
        if char == '"':
            in_quote = not in_quote
            out.append(char)
            continue
        if char == "#" and not in_quote:
            break
        out.append(char)
    return "".join(out)


def scan(path: Path, root: Path) -> list[str]:
    problems: list[str] = []
    depth = 0
    stack: list[tuple[str, int]] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = strip_comment(raw)
        if (match := OPENS.match(line)) is not None:
            depth += 1
            stack.append((match.group(1), number))
            continue
        if CLOSES.match(line) is not None:
            if depth > 0:
                depth -= 1
                stack.pop()
            continue
        if depth > 0 and TRAP.search(line):
            opener, opened_at = stack[-1]
            problems.append(
                f"{path.relative_to(root).as_posix()}:{number}: "
                f"CMAKE_CURRENT_LIST_DIR inside {opener}() opened at line {opened_at} "
                f"-- resolves against the caller's directory; freeze it at file scope "
                f"or use CMAKE_CURRENT_FUNCTION_LIST_DIR"
            )
    if depth != 0:
        problems.append(
            f"{path.relative_to(root).as_posix()}: unbalanced function()/macro() "
            f"blocks (depth {depth} at end of file)"
        )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    module_dir = root / "cmake"
    if not module_dir.is_dir():
        print(f"missing directory: {module_dir}", file=sys.stderr)
        return 2

    files = sorted(module_dir.rglob("*.cmake"))
    if not files:
        print(f"no *.cmake files under {module_dir}", file=sys.stderr)
        return 2

    problems: list[str] = []
    for path in files:
        problems.extend(scan(path, root))

    for problem in problems:
        print(problem)

    print(
        f"scanned {len(files)} cmake module(s): "
        f"{'OK' if not problems else f'{len(problems)} problem(s)'}"
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
