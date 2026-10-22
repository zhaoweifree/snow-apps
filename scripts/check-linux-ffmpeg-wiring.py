#!/usr/bin/env python3
"""Check that FFmpeg is wired up for the Linux build.

The Rust recording/export chain depends on ``ffmpeg-next``, whose build script
(``ffmpeg-sys-next``) resolves the FFmpeg libraries in this order:

  1. the ``build`` feature (compile FFmpeg from source);
  2. the ``FFMPEG_DIR`` environment variable;
  3. vcpkg;
  4. pkg-config.

Step 2 wins over step 4, and it looks at ``<FFMPEG_DIR>/include`` **only** --
it never falls back to pkg-config. So a single stale ``FFMPEG_DIR`` pointing at
a directory that does not exist turns a machine with ``libavfilter-dev``
installed into a build that fails with::

    check.c:771:18: fatal error: libavfilter/avfilter.h: No such file or directory

That is exactly what CI run 39707846164 hit: ``SNOW_FFMPEG_ROOT`` defaulted to
``<repo>/.tools/vcpkg/installed/x64-linux``, vcpkg never runs on Linux, and the
directory does not exist. The error names a header, so it reads like a missing
distribution package rather than a misrouted search path.

The C++ side has the mirror-image problem: the FFmpeg ``pkg_check_modules``
call and the links that follow it used to sit inside ``if(WIN32)``, so on Linux
nothing put ``libav*`` on the link line even though the Rust archive carries
the ``av*`` symbols.

Checked:

  * ``cmake/SnowWorkspace.cmake`` does not default ``SNOW_FFMPEG_ROOT`` to a
    vcpkg path on Linux;
  * ``cmake/RustStaticLibrary.cmake`` does not derive ``SNOW_FFMPEG_ROOT`` from
    the vcpkg tree on Linux, and only exports ``FFMPEG_DIR`` when the prefix is
    non-empty -- an empty value still counts as "set" for ``env::var()``;
  * ``snow_shot/CMakeLists.txt`` probes FFmpeg on Linux too, and the module
    list matches what ``ffmpeg-sys-next`` probes;
  * the link statements that put ``libav*`` on the link line are not inside a
    ``WIN32`` guard;
  * ``snow-crates/native/CMakeLists.txt`` lists ``libavfilter`` -- the Rust
    side links it, so a C library that omits it leaves the symbols unresolved;
  * the CI workflow installs the ``libav*-dev`` packages, and
    ``docs-uos-linux-port.md`` §9.1 lists them too.

**How the CMake branches are located.** Both CMake files contain earlier Linux
branches that have nothing to do with FFmpeg (the vcpkg triplet default, the
Rust target detection), so a plain ``re.search`` for the first
``CMAKE_SYSTEM_NAME STREQUAL "Linux"`` inspects the wrong block and passes for
the wrong reason. This script parses the if/elseif/else/endif nesting instead
and looks at the branches it actually cares about.

**How a Windows-only guard is detected.** ``if(TARGET PkgConfig::SNOW_SHOT_FFMPEG)``
occurs five times in ``snow_shot/CMakeLists.txt``, so a substring test for it
proves nothing: adding ``AND WIN32`` to one occurrence leaves four others to
satisfy the test. The script therefore records the *guard chain* around every
branch and asserts that no guard on the way to a link statement mentions
``WIN32``.

Usage: ``python scripts/check-linux-ffmpeg-wiring.py``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKSPACE = REPO / "cmake" / "SnowWorkspace.cmake"
RUST_LIBRARY = REPO / "cmake" / "RustStaticLibrary.cmake"
SHOT_CMAKE = REPO / "snow_shot" / "CMakeLists.txt"
NATIVE_CMAKE = REPO / "snow-crates" / "native" / "CMakeLists.txt"
WORKFLOW = REPO / ".github" / "workflows" / "snow-shot-linux.yml"
PORT_DOC = REPO / "docs-uos-linux-port.md"

# The predicate that selects a Linux branch.
LINUX_PREDICATE = 'CMAKE_SYSTEM_NAME STREQUAL "Linux"'

# The modules ffmpeg-sys-next's build script probes through pkg-config:
# libavutil and libavcodec unconditionally, plus one per enabled feature.
# `filter` is enabled (snow-crates/Cargo.toml: ffmpeg-next with
# codec/format/filter/software-resampling/software-scaling), so libavfilter
# is required -- without it the build script exits with "The libavfilter
# library is required but could not be found".
REQUIRED_FFMPEG_MODULES = (
    "libavcodec",
    "libavfilter",
    "libavformat",
    "libavutil",
    "libswresample",
    "libswscale",
)

# The distribution packages that provide those .pc files.
REQUIRED_APT_PACKAGES = (
    "libavcodec-dev",
    "libavfilter-dev",
    "libavformat-dev",
    "libavutil-dev",
    "libswresample-dev",
    "libswscale-dev",
)

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def strip_comments(text: str) -> str:
    """Drop CMake comments so a check cannot be satisfied by prose.

    The failure this script guards against was introduced by a *comment* that
    described the old behaviour correctly; matching that text would make the
    gate pass while the code was wrong.
    """
    without_block = re.sub(r"#\[=*\[.*?\]=*\]", "", text, flags=re.DOTALL)
    return "\n".join(re.sub(r"#.*$", "", line) for line in without_block.split("\n"))


# --------------------------------------------------------------------------
# A small CMake block parser.
#
# It tracks the if/elseif/else/endif nesting and records, for every branch,
# the guard chain that leads to it. That chain is what makes it possible to
# say "this link statement is inside a WIN32 guard" instead of "the file
# mentions WIN32 somewhere".
# --------------------------------------------------------------------------

_QUOTED = re.compile(r'"[^"]*"')
_IF_START = re.compile(r"^if\s*\(")
_ELSEIF_START = re.compile(r"^elseif\s*\(")
_ELSE_START = re.compile(r"^else\s*(\(\s*\))?$")
_ENDIF_START = re.compile(r"^endif\s*(\(\s*\))?$")


def _balance(lines: list[str]) -> int:
    """Unclosed parenthesis count of a command, ignoring quoted strings."""
    cleaned = _QUOTED.sub('""', " ".join(lines))
    return cleaned.count("(") - cleaned.count(")")


def _consume_command(lines: list[str], start: int) -> tuple[str, int]:
    """Read a possibly multi-line command; return (text, next line index)."""
    accumulated: list[str] = []
    index = start
    while index < len(lines):
        accumulated.append(lines[index].strip())
        index += 1
        if _balance(accumulated) <= 0:
            break
    return " ".join(accumulated), index


class _Branch:
    __slots__ = ("guard", "body", "scope")

    def __init__(self, guard: str, body: list[str], scope: list[str]) -> None:
        self.guard = guard
        self.body = body
        self.scope = scope


def _collect(text: str) -> list[_Branch]:
    """Return every if/elseif/else branch of a CMake file."""
    lines = text.split("\n")
    stack: list[dict] = []
    scope: list[str] = []
    result: list[_Branch] = []

    index = 0
    while index < len(lines):
        stripped = lines[index].strip()

        if _IF_START.match(stripped):
            guard, index = _consume_command(lines, index)
            stack.append({"guard": guard, "body": [], "branches": []})
            scope.append(guard)
            continue

        if _ELSEIF_START.match(stripped):
            guard, index = _consume_command(lines, index)
            if stack:
                frame = stack[-1]
                result.append(_Branch(frame["guard"], frame["body"], list(scope)))
                frame["body"] = []
                frame["guard"] = guard
                scope[-1] = guard
            continue

        if _ELSE_START.match(stripped):
            index += 1
            if stack:
                frame = stack[-1]
                result.append(_Branch(frame["guard"], frame["body"], list(scope)))
                frame["body"] = []
                frame["guard"] = "else()"
                scope[-1] = "else()"
            continue

        if _ENDIF_START.match(stripped):
            index += 1
            if stack:
                frame = stack.pop()
                result.append(_Branch(frame["guard"], frame["body"], list(scope)))
                scope.pop()
            continue

        index += 1
        if stack:
            stack[-1]["body"].append(lines[index - 1])

    while stack:
        frame = stack.pop()
        result.append(_Branch(frame["guard"], frame["body"], list(scope)))
        scope.pop()

    return result


def linux_blocks(text: str) -> list[str]:
    """Bodies of every Linux-specific branch."""
    return [
        "\n".join(branch.body)
        for branch in _collect(text)
        if LINUX_PREDICATE in branch.guard
    ]


def scope_around(text: str, needle: str) -> list[list[str]]:
    """Guard chains of the branches whose body contains ``needle``."""
    return [
        branch.scope
        for branch in _collect(text)
        if needle in "\n".join(branch.body)
    ]


_BLOCK_PAIRS = {
    "if": "endif",
    "foreach": "endforeach",
    "while": "endwhile",
    "function": "endfunction",
    "macro": "endmacro",
}


def _command_balance(text: str) -> tuple[int, int]:
    """Count block-opening commands and their end keywords in a CMake file.

    ``elseif()`` is a branch of the enclosing ``if()``, not a new block, so it
    must not be counted as an opening. Every block type is counted, because a
    parser that loses track of one of them would mis-locate every branch
    after it.
    """
    opens = 0
    closes = 0
    end_keywords = tuple(sorted(_BLOCK_PAIRS.values(), key=len, reverse=True))
    for line in text.split("\n"):
        stripped = line.strip()
        if not stripped:
            continue
        head = re.match(r"([A-Za-z_][A-Za-z0-9_]*)", stripped)
        if head is None:
            continue
        name = head.group(1)
        if name in _BLOCK_PAIRS:
            opens += 1
        elif name in end_keywords:
            closes += 1
    return opens, closes


def main() -> int:
    workspace = read(WORKSPACE)
    rust_library = read(RUST_LIBRARY)
    shot_cmake = read(SHOT_CMAKE)
    native_cmake = read(NATIVE_CMAKE)
    workflow = read(WORKFLOW)
    port_doc = read(PORT_DOC)

    # 0. The parser must stay in sync with the files it reads. If the if/endif
    #    counts disagree, every branch boundary after the damage is fiction and
    #    the checks below would quietly inspect the wrong blocks.
    for label, code in (
        ("cmake/SnowWorkspace.cmake", workspace),
        ("cmake/RustStaticLibrary.cmake", rust_library),
        ("snow_shot/CMakeLists.txt", shot_cmake),
    ):
        opens, closes = _command_balance(strip_comments(code))
        check(
            f"{label} parses with balanced if/endif",
            opens == closes,
            f"{opens} if() commands but {closes} endif() commands",
        )

    # 1. The Linux default for SNOW_FFMPEG_ROOT must not be a vcpkg path.
    workspace_code = strip_comments(workspace)
    workspace_ffmpeg = [
        block for block in linux_blocks(workspace_code) if "SNOW_FFMPEG_ROOT" in block
    ]
    check(
        "SnowWorkspace.cmake branches on Linux for the FFmpeg prefix",
        len(workspace_ffmpeg) == 1,
        f"expected exactly one Linux branch that sets SNOW_FFMPEG_ROOT, "
        f"found {len(workspace_ffmpeg)}",
    )
    for branch in workspace_ffmpeg:
        check(
            "the Linux SNOW_FFMPEG_ROOT default does not point at the vcpkg tree",
            "SNOW_VCPKG_INSTALLED_DIR" not in branch
            and "VCPKG_TARGET_TRIPLET" not in branch
            and "SNOW_VCPKG_ROOT" not in branch,
            "the Linux branch still derives the prefix from vcpkg",
        )
        check(
            "the Linux SNOW_FFMPEG_ROOT default is empty",
            re.search(r'set\(SNOW_FFMPEG_ROOT\s+""', branch) is not None,
            'an empty prefix is the signal for "let pkg-config find it"',
        )

    # 2. The Rust library must not fall back to vcpkg on Linux either, and it
    #    must not export an empty FFMPEG_DIR.
    rust_code = strip_comments(rust_library)
    rust_ffmpeg = [
        block for block in linux_blocks(rust_code) if "SNOW_FFMPEG_ROOT" in block
    ]
    check(
        "RustStaticLibrary.cmake branches on Linux for the FFmpeg prefix",
        len(rust_ffmpeg) == 1,
        f"expected exactly one Linux branch that sets SNOW_FFMPEG_ROOT, "
        f"found {len(rust_ffmpeg)}",
    )
    for branch in rust_ffmpeg:
        check(
            "the Linux SNOW_FFMPEG_ROOT derivation does not reach for vcpkg",
            "VCPKG_INSTALLED_DIR" not in branch
            and "SNOW_VCPKG_ROOT" not in branch
            and "VCPKG_TARGET_TRIPLET" not in branch,
            "the Linux branch still derives the prefix from vcpkg",
        )
        check(
            "the Linux SNOW_FFMPEG_ROOT derivation leaves the prefix empty",
            re.search(r'set\(SNOW_FFMPEG_ROOT\s+""', branch) is not None,
            "the Linux branch must keep the prefix empty so pkg-config is used",
        )
    # The guard has to sit immediately in front of the append: an empty value
    # still selects ffmpeg-sys-next's prebuilt branch, because the build script
    # branches on `env::var("FFMPEG_DIR")` succeeding.
    check(
        "RustStaticLibrary.cmake only exports FFMPEG_DIR when the prefix is set",
        re.search(
            r'if\(NOT SNOW_FFMPEG_ROOT STREQUAL ""\)\s*\n\s*'
            r'list\(APPEND _cargo_environment "FFMPEG_DIR=',
            rust_code,
        )
        is not None,
        "FFMPEG_DIR is exported unconditionally; an empty value still selects "
        "ffmpeg-sys-next's prebuilt branch and bypasses pkg-config",
    )

    # 3. The C++ side must probe FFmpeg on Linux, with the module list the
    #    Rust build script probes.
    shot_code = strip_comments(shot_cmake)
    shot_ffmpeg = [
        block
        for block in linux_blocks(shot_code)
        if "pkg_check_modules(SNOW_SHOT_FFMPEG" in block
    ]
    check(
        "snow_shot/CMakeLists.txt probes FFmpeg on Linux",
        len(shot_ffmpeg) == 1,
        f"expected exactly one Linux branch calling "
        f"pkg_check_modules(SNOW_SHOT_FFMPEG ...), found {len(shot_ffmpeg)}",
    )
    for branch in shot_ffmpeg:
        check(
            "the Linux branch requires pkg-config",
            "find_package(PkgConfig REQUIRED)" in branch,
            "the distribution FFmpeg is found through pkg-config",
        )
    module_lines = re.findall(r"set\(_snow_ffmpeg_modules ([^\n]*)\)", shot_code)
    check(
        "the FFmpeg module list is declared for the Linux branch too",
        len(module_lines) >= 2,
        f"expected one module list per platform, found {len(module_lines)}",
    )
    for line in module_lines:
        missing = [module for module in REQUIRED_FFMPEG_MODULES if module not in line]
        check(
            "every module ffmpeg-sys-next probes is in the CMake module list",
            not missing,
            "missing: " + ", ".join(missing),
        )

    # 4. The link statements must not sit behind a Windows-only guard.
    for needle, label in (
        (
            "target_link_libraries(${_snow_rust_bundle} INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)",
            "the Rust FFI bundle carries the FFmpeg dependency on every platform",
        ),
        (
            "target_link_libraries(snow_shot PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)",
            "the application links FFmpeg on every platform",
        ),
    ):
        chains = scope_around(shot_code, needle)
        check(
            label,
            bool(chains),
            "the link statement is missing from snow_shot/CMakeLists.txt",
        )
        for chain in chains:
            windows_guards = [guard for guard in chain if "WIN32" in guard]
            check(
                label + " (no WIN32 guard)",
                not windows_guards,
                "guarded by " + "; ".join(windows_guards),
            )

    # 5. The C library must list libavfilter as well.
    native_code = strip_comments(native_cmake)
    native_modules = re.findall(r"pkg_check_modules\(SNOW_MEDIA_FFMPEG[^\n]*", native_code)
    check(
        "snow-crates/native/CMakeLists.txt lists the FFmpeg modules",
        bool(native_modules),
        "no pkg_check_modules(SNOW_MEDIA_FFMPEG ...) call found",
    )
    for call in native_modules:
        missing = [module for module in REQUIRED_FFMPEG_MODULES if module not in call]
        check(
            "the C library probes every module the Rust side links",
            not missing,
            "missing: " + ", ".join(missing),
        )

    # 6. The CI job and the port document must list the development packages.
    for package in REQUIRED_APT_PACKAGES:
        check(
            f"the CI workflow installs {package}",
            re.search(rf"\b{re.escape(package)}\b", workflow) is not None,
            "the package is missing from the apt list",
        )
    check(
        "the port document lists the FFmpeg development packages",
        all(re.search(rf"\b{re.escape(package)}\b", port_doc) for package in REQUIRED_APT_PACKAGES),
        "§9.1 does not list the libav*-dev packages",
    )

    if failures:
        print(f"\n{len(failures)} check(s) failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("\nall FFmpeg wiring checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
