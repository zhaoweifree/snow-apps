#!/usr/bin/env python3
"""Gate: Qt5/Qt6 divergent virtual functions must be overridden version-neutrally.

Some Qt virtual functions changed their signature between Qt 5.15 and Qt 6.12.
A subclass that spells the signature of one Qt fails to compile on the other
one with

    error: 'QVariant ...::retrieveData(const QString&, QMetaType) const'
           marked 'override', but does not override

That failure only ever shows up on the route that uses the *other* spelling,
which is how QMimeData::retrieveData stayed broken: the overriding class sits
behind `#if !defined(Q_OS_WIN)`, so the Windows build (Qt 6) never compiled it
and the Linux build (Qt 5) hit it first (CI run 39736061339).

The fix is to spell the parameter through a shim alias in
snow_shot/include/snow_shot/compat/qt_compat.h. This gate makes sure every
override of a known-divergent virtual uses such an alias (or sits inside an
explicit version guard), so the same defect cannot be reintroduced anywhere
else.

The divergent signatures below were established by diffing the two local Qt
header trees (Qt 5.15.2 vs Qt 6.12.0) over the classes this project derives
from; the source of truth is the header, not this list, so the list is
re-derivable and this gate fails if the tree grows a new divergent override
without an entry.

Run: python scripts/check-qt5-qt6-virtual-signatures.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPAT_HEADER = REPO / "snow_shot" / "include" / "snow_shot" / "compat" / "qt_compat.h"

SOURCE_ROOTS = ("snow_shot", "snow_image", "snow_draw_engine_qt", "snow_memory")

# --- The divergence table ---------------------------------------------------
#
# class name -> (method name, qt5 parameter list, qt6 parameter list,
#                 shim alias that spells it version-neutrally)
#
# Every row was read out of the two local Qt header trees:
#   Qt 5.15.2 -> C:\Qt\5.15.2\msvc2019_64\include
#   Qt 6.12.0 -> C:\Qt\6.12.0\msvc2022_64\include
DIVERGENT_VIRTUALS = {
    "QMimeData": (
        "retrieveData",
        "QVariant::Type",
        "QMetaType",
        "SnowMimeDataType",
    ),
    "QAbstractNativeEventFilter": (
        "nativeEventFilter",
        "long",
        "qintptr",
        "snowNativeEventResult",
    ),
}

# --- Source scanning --------------------------------------------------------

OVERRIDE_RE = re.compile(
    r"^\s*(?P<ret>[A-Za-z_][A-Za-z0-9_:<>,\s*&]*?)\b"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\("
    r"(?P<args>[^;{]*)\)\s*"
    r"(?P<qualifiers>(?:\s+const)?(?:\s+noexcept)?(?:\s+override)?)\s*\{",
    re.M,
)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^>"]+)[>"]', re.M)
VERSION_GUARD_RE = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*"
    r"(?:SNOW_QT5|SNOW_QT6|QT_VERSION)",
    re.M,
)


def strip_comments(text: str) -> str:
    """Blank out comments so a correct spelling inside a comment cannot pass."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def collect_sources() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            if "build" in path.parts or ".git" in path.parts:
                continue
            files.append(path)
    return files


def find_overrides(text: str) -> list[tuple[str, str, int]]:
    """Return (method name, parameter list, line number) of every override."""
    found: list[tuple[str, str, int]] = []
    for match in OVERRIDE_RE.finditer(text):
        if "override" not in match.group("qualifiers"):
            continue
        line = text.count("\n", 0, match.start()) + 1
        found.append((match.group("name"), match.group("args"), line))
    return found


def guarded_by_version(text: str, offset: int) -> bool:
    """Is the code at `offset` inside an explicit Qt version guard?

    Stack-based: every `#if` pushes whether it is a version guard, every
    `#endif` pops. The code is version-guarded when any open guard on the
    stack is a version guard.
    """
    stack: list[bool] = []
    for line in text[:offset].splitlines():
        stripped = line.strip()
        if stripped.startswith("#if"):
            stack.append(bool(VERSION_GUARD_RE.match(line)))
        elif stripped.startswith("#endif"):
            if stack:
                stack.pop()
    return any(stack)


def alias_definitions(text: str, alias: str) -> list[str]:
    """Every `using <alias> = ...;` body in the text, comments stripped."""
    return [
        match.group("body").strip()
        for match in re.finditer(
            rf"using\s+{re.escape(alias)}\s*=\s*(?P<body>[^;]+);", text
        )
    ]


def offset_of_line(text: str, line: int) -> int:
    """Character offset of the start of 1-based `line`."""
    offset = 0
    for _ in range(line - 1):
        newline = text.find("\n", offset)
        if newline < 0:
            return len(text)
        offset = newline + 1
    return offset


def main() -> int:
    failures: list[str] = []

    def check(label: str, ok: bool, detail: str) -> None:
        if ok:
            print(f"ok   {label}")
        else:
            print(f"FAIL {label}")
            print(f"     {detail}")
            failures.append(label)

    # --- the shim aliases must exist -------------------------------------
    compat = COMPAT_HEADER.read_text(encoding="utf-8", errors="replace")
    compat_stripped = strip_comments(compat)
    for cls, (method, qt5_type, qt6_type, alias) in sorted(DIVERGENT_VIRTUALS.items()):
        check(
            f"{cls}::{method} has a shim alias in the compat header",
            re.search(rf"using\s+{re.escape(alias)}\s*=", compat_stripped) is not None,
            f"the alias {alias} is missing from {COMPAT_HEADER.name}; "
            f"without it the {method} override cannot be spelled version-neutrally",
        )
        # The alias must name the Qt5 type on one branch and the Qt6 type on
        # the other. The compat header spells that as
        #     #if SNOW_QT6
        #     using <alias> = <qt6 type>;
        #     #else
        #     using <alias> = <qt5 type>;
        #     #endif
        # so both bodies have to be present.
        bodies = alias_definitions(compat_stripped, alias)
        check(
            f"{cls}::{method} shim alias covers both Qt versions",
            len(bodies) >= 2
            and any(qt5_type in body for body in bodies)
            and any(qt6_type in body for body in bodies),
            f"the alias {alias} is defined as {bodies!r}; it must name {qt5_type} "
            f"on the Qt5 branch and {qt6_type} on the Qt6 branch",
        )

    # --- every override of a divergent virtual must be version-neutral ----
    sources = collect_sources()
    check("the source tree was scanned", bool(sources), "no source files were found")

    offenders: list[str] = []
    for path in sources:
        try:
            raw = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        text = strip_comments(raw)
        for name, args, line in find_overrides(text):
            entry = None
            for cls, (method, qt5_type, qt6_type, alias) in DIVERGENT_VIRTUALS.items():
                if method == name:
                    entry = (cls, method, qt5_type, qt6_type, alias)
                    break
            if entry is None:
                continue
            cls, method, qt5_type, qt6_type, alias = entry
            if alias in args:
                continue
            # Locate the override itself: the scan reports the line, so the
            # offset is derived from the line number rather than from the
            # first occurrence of the parameter list in the file.
            offset = offset_of_line(text, line)
            if guarded_by_version(text, offset):
                continue
            rel = path.relative_to(REPO)
            offenders.append(
                f"{rel}:{line}: {cls}::{method} is overridden with "
                f"'{args.strip()}' -- the Qt5 type is {qt5_type}, the Qt6 type is "
                f"{qt6_type}; spell it as {alias} or guard it with "
                f"#if SNOW_QT5 / #if SNOW_QT6"
            )

    check(
        "every override of a Qt5/Qt6 divergent virtual is version-neutral",
        not offenders,
        "\n     ".join(offenders) if offenders else "",
    )

    print()
    if failures:
        print(f"FAILED: {len(failures)} problem(s)")
        return 1
    print("qt5/qt6 virtual signature check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
