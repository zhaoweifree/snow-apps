#!/usr/bin/env python3
"""Check that the Qt compat shim never tests an undefined macro.

`snow_shot/include/snow_shot/compat/qt_compat.h` is force-included into every
C++ translation unit of the project (Qt5 route: `-include` / `/FI`).  It
therefore has to be correct on *every* compiler, including the ones the
project never runs locally.

CI run 39715098658 (Linux x64) failed with::

    qt_compat.h:91:5: error: "SNOW_QT5" is not defined, evaluates to 0 [-Werror=undef]

The cause was an ordering bug, not a typo: the version macros were defined
*below* the first `#if` that tested them::

    #if SNOW_QT5              // <-- SNOW_QT5 not defined yet
    #  include <QVector>
    #endif
    ...
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #  define SNOW_QT6 1
    #  define SNOW_QT5 0      // <-- defined far too late

An undefined identifier in `#if` evaluates to 0, so the Qt5 branch of
`<QVector>` never ran anywhere, and `SnowRoleList` only compiled because
`<QList>` drags in `qvector.h` by itself.  MSVC and GCC disagree about
whether that is an error, so the defect was invisible locally.

Two rules are checked here:

  1. **Order.**  Every macro that this header itself defines must be defined
     before the first `#if`/`#elif` that tests it.  The check tracks
     `#define`/`#undef` per header and reports any test of a macro that is
     not yet defined (and is not provided by the toolchain, i.e. not in the
     allowlist of compiler/standard/Qt-provided names).
  2. **Coverage.**  The two version macros `SNOW_QT5`/`SNOW_QT6` must both be
     defined unconditionally, and the Qt5 `<QVector>` include must be
     reachable -- i.e. guarded by `SNOW_QT5`, which is now defined above it.
     A header that defines the macros but keeps a *later* copy of the version
     test would pass rule 1 while still evaluating `SNOW_QT5` against an
     undefined name in a fresh translation unit.

Both rules are *textual* on purpose: the failure mode they guard against is
an ordering mistake in the preprocessor, and the project has no GCC on the
developer machines, so a compile-based gate could not run at all.

Usage: ``python scripts/check-qt-compat-macro-order.py``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPAT_HEADER = REPO / "snow_shot" / "include" / "snow_shot" / "compat" / "qt_compat.h"

# Macros that the toolchain or Qt provide, so this header never defines them
# and never has to.  Testing one of these is legitimate even though the
# header does not define it.
EXTERNAL_MACROS = frozenset(
    {
        # C++ standard / compiler built-ins.
        "__cplusplus",
        "__has_include",
        "__has_cpp_attribute",
        "__has_builtin",
        "__has_feature",
        "__cpp_lib_chrono",
        # Qt-provided.
        "QT_VERSION",
        "QT_VERSION_CHECK",
        "Q_OS_LINUX",
        "Q_OS_UNIX",
        "Q_OS_WIN",
        "Q_BYTE_ORDER",
        "Q_UNREACHABLE_RETURN",
        "Q_DECL_CONSTEXPR",
        "Q_DECL_CONSTEXPR_INLINE",
        "Q_DECL_CONSTEXPR_TEMPLATE",
    }
)

# `#if`/`#elif` conditions.  The leading `#` is required so that a `#` inside
# a string literal in the header cannot be mistaken for a directive.
DIRECTIVE_RE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|endif|else|define|undef)\b(.*)$")
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
UNDEF_RE = re.compile(r"^[ \t]*#[ \t]*undef[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
# A bare identifier used as a macro test: `#if FOO`, `#if !FOO`, `#if FOO && BAR`.
# `defined(FOO)` is handled separately because it is legal to test an
# undefined macro that way.
BARE_TEST_RE = re.compile(r"(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z0-9_]*)")
DEFINED_CALL_RE = re.compile(r"defined[ \t]*\([ \t]*([A-Za-z_][A-Za-z0-9_]*)")
# `FOO(...)` is a function-like macro invocation, not a value test.
FUNC_CALL_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def strip_comments(text: str) -> str:
    """Remove comments so a check cannot be satisfied by prose.

    The defect this script guards against was accompanied by a comment that
    described the *intent* correctly; matching that text would let the gate
    pass while the code was wrong.
    """
    out: list[str] = []
    in_block = False
    for line in text.split("\n"):
        stripped = line
        if in_block:
            end = stripped.find("*/")
            if end < 0:
                continue
            stripped = " " * end + stripped[end + 2 :]
            in_block = False
        while "/*" in stripped:
            start = stripped.find("/*")
            end = stripped.find("*/", start + 2)
            if end < 0:
                stripped = stripped[:start] + " " * (len(stripped) - start)
                in_block = True
                break
            stripped = stripped[:start] + " " * (end + 2 - start) + stripped[end + 2 :]
        # A line comment ends the line, but a backslash before the newline
        # continues it -- the header uses that for multi-line #if conditions.
        hash_pos = stripped.find("//")
        if hash_pos >= 0:
            stripped = stripped[:hash_pos]
        out.append(stripped)
    return "\n".join(out)


def tested_identifiers(condition: str) -> set[str]:
    """Return the macro names a `#if`/`#elif` condition tests as values.

    `defined(FOO)` is excluded: it is the legal way to ask whether a macro
    exists.  `FOO(...)` is excluded: that is a function-like invocation, and
    `__has_include(<Header>)` falls into that bucket -- the header name it
    carries is a file name, not an identifier.
    """
    without_defined = DEFINED_CALL_RE.sub(" ", condition)
    # Remove the whole `__has_include(<...>)` argument list first: the header
    # name inside the angle brackets is a file name, not an identifier.
    without_defined = re.sub(r"__has_include[ \t]*\([^)]*\)", " ", without_defined)
    names: set[str] = set()
    for match in BARE_TEST_RE.finditer(without_defined):
        name = match.group(1)
        if name in ("defined", "true", "false"):
            continue
        names.add(name)
    # Drop names that are used as function-like macro calls.
    for match in FUNC_CALL_RE.finditer(without_defined):
        names.discard(match.group(1))
    return names


def main() -> int:
    if not COMPAT_HEADER.is_file():
        print(f"FAIL compat header not found: {COMPAT_HEADER}")
        return 1

    raw = COMPAT_HEADER.read_text(encoding="utf-8", errors="replace")
    code = strip_comments(raw)
    lines = code.split("\n")

    # Rule 1: a macro defined by this header must be defined before the first
    # test of it.  `defined` is tracked per header, not per translation unit.
    defined: set[str] = set()
    first_define_line: dict[str, int] = {}
    offending: list[tuple[int, str, str]] = []
    pending_conditions: list[tuple[int, str]] = []

    for number, line in enumerate(lines, start=1):
        match = DIRECTIVE_RE.match(line)
        if match is None:
            continue
        directive = match.group(1)
        rest = match.group(2)

        if directive == "define":
            define_match = DEFINE_RE.match(line)
            if define_match is not None:
                name = define_match.group(1)
                if name not in first_define_line:
                    first_define_line[name] = number
                defined.add(name)
            continue

        if directive == "undef":
            undef_match = UNDEF_RE.match(line)
            if undef_match is not None:
                defined.discard(undef_match.group(1))
            continue

        if directive in ("if", "elif", "ifdef", "ifndef"):
            pending_conditions.append((number, rest))
            for name in tested_identifiers(rest):
                if name in EXTERNAL_MACROS:
                    continue
                if name not in defined:
                    offending.append((number, name, directive))
            # `ifdef`/`ifndef` test existence, which is always legal.
            if directive in ("ifdef", "ifndef"):
                pending_conditions.pop()
            continue

        if directive in ("else", "endif"):
            pending_conditions.clear()

    check(
        "compat header tests no undefined macro",
        not offending,
        "; ".join(
            f"line {number}: `{name}` is not defined before the #{kind}"
            for number, name, kind in offending[:8]
        ),
    )

    # Rule 2: the version macros must exist, be defined unconditionally, and
    # the Qt5 <QVector> include must be reachable through SNOW_QT5.
    check(
        "SNOW_QT5 and SNOW_QT6 are both defined in the compat header",
        "SNOW_QT5" in first_define_line and "SNOW_QT6" in first_define_line,
        f"SNOW_QT5 at {first_define_line.get('SNOW_QT5')}, "
        f"SNOW_QT6 at {first_define_line.get('SNOW_QT6')}",
    )

    vector_include_line = None
    for number, line in enumerate(lines, start=1):
        if re.search(r"^[ \t]*#[ \t]*include[ \t]*<QVector>", line):
            vector_include_line = number
            break
    check(
        "the compat header includes <QVector> for the Qt5 branch",
        vector_include_line is not None,
        "no `#include <QVector>` found",
    )

    if vector_include_line is not None:
        # The guarding `#if SNOW_QT5` has to sit above the include, and the
        # include has to sit above the point where SNOW_QT5 is defined --
        # otherwise the include is dead code, which is exactly the defect
        # this script exists to catch.
        guard_line = None
        for number in range(vector_include_line - 1, 0, -1):
            if re.match(r"^[ \t]*#[ \t]*if[ \t]+SNOW_QT5\b", lines[number - 1]):
                guard_line = number
                break
        check(
            "the <QVector> include is guarded by SNOW_QT5",
            guard_line is not None,
            f"no `#if SNOW_QT5` above line {vector_include_line}",
        )
        if guard_line is not None:
            define_line = first_define_line.get("SNOW_QT5")
            check(
                "SNOW_QT5 is defined before the <QVector> include that tests it",
                define_line is not None and define_line < guard_line,
                f"SNOW_QT5 defined at line {define_line}, "
                f"tested at line {guard_line}",
            )

    # Rule 3: the two version macros must be defined in exactly one `#if`
    # block, and each branch of that block must define both of them.  A
    # per-branch definition (`SNOW_QT6=1` / `SNOW_QT5=0` versus
    # `SNOW_QT6=0` / `SNOW_QT5=1`) is the correct shape; what must not
    # happen is a *second, independent* definition block that could disagree
    # with the first.
    qt5_defines = [
        number
        for number, line in enumerate(lines, start=1)
        if re.match(r"^[ \t]*#[ \t]*define[ \t]+SNOW_QT5\b", line)
    ]
    qt6_defines = [
        number
        for number, line in enumerate(lines, start=1)
        if re.match(r"^[ \t]*#[ \t]*define[ \t]+SNOW_QT6\b", line)
    ]
    check(
        "SNOW_QT5 and SNOW_QT6 are defined in the same #if block",
        qt5_defines and qt6_defines and max(qt5_defines) - min(qt6_defines) < 8
        and max(qt6_defines) - min(qt5_defines) < 8,
        f"SNOW_QT5 at {qt5_defines}, SNOW_QT6 at {qt6_defines}",
    )
    check(
        "every SNOW_QT5/SNOW_QT6 definition is a 0/1 literal",
        all(
            re.match(r"^[ \t]*#[ \t]*define[ \t]+SNOW_QT[56][ \t]+[01][ \t]*$", lines[number - 1])
            for number in qt5_defines + qt6_defines
        ),
        "a version macro is defined to something other than 0 or 1",
    )

    if failures:
        print()
        print(f"{len(failures)} check(s) failed:")
        for label in failures:
            print(f"  - {label}")
        return 1

    print()
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
