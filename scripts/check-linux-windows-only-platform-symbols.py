#!/usr/bin/env python3
"""Platform-only platform symbols must not be called from unguarded code.

`snow_shot/include/snow_shot/platform/windowcaptureexclusion.h` declares
`snow_shot::platform::setWindowExcludedFromCapture` and `captureWindowId`. Both
are defined in `src/platform/windows/windowchrome.cpp`, and the whole file is
inside `#if defined(Q_OS_WIN)`. A translation unit that includes the header only
on Windows and then calls the functions unconditionally compiles fine there and
fails on Linux with

    error: 'captureWindowId' is not a member of 'snow_shot::platform'
    error: 'setWindowExcludedFromCapture' is not a member of 'snow_shot::platform'
                                                    (CI run 24, arm64, 594/608)

This is the same blind spot as 9.6.19 and 9.6.24: the platform that can compile
the code is not the platform that consumes it. Nothing on the workstation can
see it, and each occurrence costs a thirty-minute CI round trip.

The gate:

  * reads every `snow_shot/src/platform/windows/*.cpp` and records the function
    definitions that sit inside an active Windows preprocessor branch, in the
    `snow_shot::platform` namespace only -- the `::windows` namespace is called
    with a qualified name and is already guarded everywhere;
  * finds every other file that names one of those symbols;
  * requires the naming line to be inside a branch whose condition mentions
    `Q_OS_WIN`/`_WIN32`/`Q_OS_MACOS` (the two platforms that ship an
    implementation), tracking `#if`/`#elif`/`#else`/`#endif` nesting so an
    `#else` of a Windows guard does not count as guarded.

Usage: ``python scripts/check-linux-windows-only-platform-symbols.py``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLATFORM_DIR = REPO / "snow_shot" / "src" / "platform"
INCLUDE_PLATFORM_DIR = REPO / "snow_shot" / "include" / "snow_shot" / "platform"

# Directories whose contents are allowed to name these symbols freely: the
# definition itself, its private header, and the platform-specific subtrees.
EXEMPT_PREFIXES = (
    "snow_shot/src/platform/windows/",
    "snow_shot/include/snow_shot/platform/windows/",
    "snow_shot/src/platform/macos/",
    "snow_shot/include/snow_shot/platform/macos/",
    "snow_shot/src/platform/linux/",
    "snow_shot/include/snow_shot/platform/linux/",
)

GUARD_CONDITION = re.compile(r"Q_OS_WIN|_WIN32|WIN32|Q_OS_MACOS|Q_OS_MACOSX|APPLE")
NEGATED_GUARD = re.compile(r"\bNOT\s+(?:WIN32|APPLE)\b")
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
# `type name(` at the start of a line, allowing attributes and pointer returns.
FUNCTION_DEFINITION = re.compile(
    r"^(?:\[\[[^\]]*\]\]\s*)*[A-Za-z_][A-Za-z0-9_:<>,\s*&]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\("
)
USE_PATTERN = r"\b{name}\b"

failures: list[str] = []


def fail(message: str) -> None:
    global failures
    print(f"FAIL: {message}")
    failures.append(message)


class Branch:
    """One side of a preprocessor conditional, with its condition text."""

    __slots__ = ("condition", "is_first")

    def __init__(self, condition: str, is_first: bool) -> None:
        self.condition = condition
        self.is_first = is_first


def guarded_windows(stack: list[Branch]) -> bool:
    """True when some enclosing branch is the *taken* side of a Windows guard.

    `#if defined(Q_OS_WIN) ... #else ... #endif` guards its first branch only;
    the `#else` is the not-Windows side, so `is_first` has to be part of the
    answer or every `#else` would look like a guard. `if(NOT WIN32)` in a CMake
    file says the opposite of a guard, so a negated condition is skipped.
    """
    for branch in stack:
        if not branch.is_first:
            continue
        if NEGATED_GUARD.search(branch.condition):
            continue
        if GUARD_CONDITION.search(branch.condition):
            return True
    return False


def walk(path: Path):
    """Yield (line number, text, stack) for every line, tracking #if nesting."""
    stack: list[Branch] = []
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        match = DIRECTIVE.match(line)
        if match:
            directive, rest = match.group(1), match.group(2)
            if directive in ("if", "ifdef", "ifndef"):
                stack.append(Branch(rest, True))
            elif directive == "elif":
                if stack:
                    stack[-1] = Branch(rest, True)
                else:
                    stack.append(Branch(rest, True))
            elif directive == "else":
                if stack:
                    stack[-1] = Branch(stack[-1].condition, False)
            elif directive == "endif":
                if stack:
                    stack.pop()
            yield number, line, stack
            continue
        yield number, line, stack


def defined_symbols(paths: list[Path], namespace: str = "snow_shot::platform",
                    only_windows: bool = False) -> dict[str, str]:
    """{symbol: file:line} for functions defined in `namespace`.

    `only_windows` restricts the scan to branches inside a Windows guard, which
    is how a Windows-only definition is recognised.
    """
    symbols: dict[str, str] = {}
    for path in paths:
        current = None
        for number, line, stack in walk(path):
            if only_windows and not guarded_windows(stack):
                continue
            namespace_match = re.match(r"^\s*namespace\s+([A-Za-z0-9_:]+)\s*\{", line)
            if namespace_match:
                current = namespace_match.group(1)
                continue
            if current != namespace:
                continue
            definition = FUNCTION_DEFINITION.match(line)
            if definition and definition.group(1) not in ("if", "for", "while", "switch", "return"):
                symbols.setdefault(
                    definition.group(1),
                    f"{path.relative_to(REPO).as_posix()}:{number}",
                )
    return symbols


CMAKE_IF = re.compile(r"^\s*(if|elseif|else|endif)\s*\(", re.IGNORECASE)
CMAKE_ADD_TARGET = re.compile(r"^\s*add_(?:executable|library)\s*\(\s*([A-Za-z0-9_\-]+)(.*)$")


def source_files() -> list[Path]:
    files = []
    for pattern in ("snow_shot/src/**/*.cpp", "snow_shot/tests/*.cpp"):
        files.extend(sorted(REPO.glob(pattern)))
    return [
        path for path in files
        if not path.relative_to(REPO).as_posix().startswith(EXEMPT_PREFIXES)
    ]


def windows_only_target_sources() -> set[str]:
    """Sources that belong to a CMake target declared inside `if(WIN32)`/`if(APPLE)`.

    `recapture_cursor_native_e2e_test.cpp` calls the Windows-only exclusion API
    on purpose: it is only ever compiled on Windows (`if(WIN32)` in
    `snow_shot/CMakeLists.txt`). A gate that does not read the build file cannot
    tell that apart from a real mistake.
    """
    cmakelists = REPO / "snow_shot" / "CMakeLists.txt"
    if not cmakelists.is_file():
        return set()
    paths: set[str] = set()
    stack: list[Branch] = []
    collecting: list[str] | None = None
    for line in cmakelists.read_text(encoding="utf-8", errors="replace").splitlines():
        match = CMAKE_IF.match(line)
        if match:
            keyword = match.group(1).lower()
            rest = line[match.end() - 1 :]
            if keyword == "if":
                stack.append(Branch(rest, True))
            elif keyword == "elseif":
                if stack:
                    stack[-1] = Branch(rest, True)
            elif keyword == "else":
                if stack:
                    stack[-1] = Branch(stack[-1].condition, False)
            elif keyword == "endif":
                if stack:
                    stack.pop()
            continue
        if collecting is not None:
            collecting.append(line)
            if ")" in line:
                body = "\n".join(collecting)
                collecting = None
                if guarded_windows(stack):
                    paths.update(
                        match.rstrip() for match in
                        re.findall(r"[A-Za-z0-9_./\-]+\.(?:cpp|mm|h)", body)
                    )
            continue
        target = CMAKE_ADD_TARGET.match(line)
        if target and guarded_windows(stack):
            collecting = [target.group(2)]
            if ")" in target.group(2):
                body = target.group(2)
                collecting = None
                paths.update(re.findall(r"[A-Za-z0-9_./\-]+\.(?:cpp|mm|h)", body))
    return paths


def main() -> int:
    if not (PLATFORM_DIR / "windows").is_dir():
        fail("snow_shot/src/platform/windows is missing -- this gate has lost its subject")
        return 1

    windows_symbols = defined_symbols(sorted((PLATFORM_DIR / "windows").glob("*.cpp")),
                                      only_windows=True)
    # A symbol that also has a Linux definition (or an unguarded cross-platform
    # one) is reachable there, so it is not Windows-only. `focusedFullscreen*`
    # is exactly that: three definitions, one per platform.
    reachable = defined_symbols(sorted((PLATFORM_DIR / "linux").glob("*.cpp")))
    reachable.update(defined_symbols(sorted(PLATFORM_DIR.glob("*.cpp"))))
    symbols = {name: origin for name, origin in windows_symbols.items() if name not in reachable}
    if not symbols:
        fail(
            "no Windows-only symbol was found in snow_shot::platform; the parser or the "
            "source layout changed and this gate would pass everything"
        )
        return 1

    windows_target_sources = windows_only_target_sources()

    uses = 0
    for path in source_files():
        relative = path.relative_to(REPO).as_posix()
        if relative in windows_target_sources or relative.removeprefix("snow_shot/") in windows_target_sources:
            continue
        for number, line, stack in walk(path):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for name, origin in symbols.items():
                if not re.search(USE_PATTERN.format(name=name), line):
                    continue
                uses += 1
                if guarded_windows(stack):
                    continue
                fail(
                    f"{relative}:{number} names platform::{name}, which is only defined "
                    f"under a Windows guard ({origin}); on Linux the symbol does not "
                    "exist. Wrap the call in the platform guard, or degrade explicitly."
                )

    if uses == 0:
        fail("no use of a Windows-only symbol was found -- the search never matched")
        return 1

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print(
        f"\nWindows-only platform symbol check passed "
        f"({len(symbols)} symbol(s): {', '.join(sorted(symbols))}; {uses} guarded use(s))"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
