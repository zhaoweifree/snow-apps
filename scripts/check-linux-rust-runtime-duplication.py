#!/usr/bin/env python3
"""Two Rust staticlibs in one image: where the duplicate runtime is tolerated.

`crate-type = ["staticlib"]` bundles every dependency of the crate - including
`std` - into the archive. Windows and macOS link exactly one such archive, so
nothing ever collides there. Linux links two (`libsnow_linux_c.a` and
`libsnow_shot_rust_ffi.a`) and GNU ld refuses the second copy of the runtime:

    multiple definition of `rust_eh_personality'
    multiple definition of `std::panicking::EMPTY_PANIC'
    multiple definition of `std::sys::args::unix::imp::ARGV_INIT_ARRAY'
        (both sides are the *same* std build, same object name; CI run 29)

`-Wl,--allow-multiple-definition` is the workaround, and where it is written is
the whole point: attached to `snow_shot_linux_platform`'s INTERFACE it reaches
only the targets that pull the second archive. At directory or project scope it
would also hide a genuine duplicate definition in the 600-file C++ side, which
is exactly the kind of silent downgrade this port keeps paying for.

So this gate pins three things: the flag exists, it is attached there, and it
appears nowhere else.

Run it directly; the Linux packaging gate runs it as well.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FLAG = "--allow-multiple-definition"

SKIP_DIRECTORIES = {
    ".git",
    ".tools",
    ".workbuddy",  # reverse-validation scripts quote the flag on purpose
    "artifacts",
    "build",
    "node_modules",
    "target",
}
SCANNED_SUFFIXES = (".cmake", ".txt", ".sh", ".yml", ".yaml")

SNOW_SHOT_CMAKE = Path("snow_shot/CMakeLists.txt")

# The Rust archives that end up in one image. Kept here so the comment in
# CMakeLists.txt and the gate cannot drift apart.
RUST_ARCHIVES = ("libsnow_linux_c.a", "libsnow_shot_rust_ffi.a")

_platform_guard = re.compile(r"\bUNIX\b.*\bNOT\s+APPLE\b")


def walk_files(root: Path, suffixes: tuple[str, ...]):
    for directory, subdirectories, filenames in os.walk(root):
        subdirectories[:] = [name for name in subdirectories if name not in SKIP_DIRECTORIES]
        for filename in filenames:
            if filename.endswith(suffixes):
                yield Path(directory) / filename


def guard_stack_at(lines: list[str], index: int) -> list[str]:
    """The `if()` conditions open at that line, outermost first."""
    stack: list[str] = []
    for line in lines[:index]:
        stripped = line.strip()
        if stripped.startswith("if(") or stripped.startswith("if ("):
            stack.append(stripped[stripped.find("(") + 1 : stripped.rfind(")")])
        elif stripped.startswith("elseif") or stripped.startswith("else("):
            if stack:
                stack[-1] = "or " + stack[-1]
        elif stripped.startswith("endif"):
            if stack:
                stack.pop()
    return stack


def occurrences() -> list[tuple[Path, int]]:
    found: list[tuple[Path, int]] = []
    for path in walk_files(ROOT, SCANNED_SUFFIXES):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            if FLAG in line and not line.lstrip().startswith("#"):
                found.append((path.relative_to(ROOT), number))
    return found


def main() -> int:
    failures: list[str] = []

    if not SNOW_SHOT_CMAKE.is_file():
        print(f"FAIL: {SNOW_SHOT_CMAKE} is missing", file=sys.stderr)
        return 1

    lines = SNOW_SHOT_CMAKE.read_text(encoding="utf-8").splitlines()

    # 1. Exactly one occurrence, in snow_shot/CMakeLists.txt.
    found = occurrences()
    if not found:
        failures.append(
            f"{FLAG} is not in the tree: two Rust staticlibs in one link will stop "
            "the build at the link step (CI run 29)")
    for path, number in found:
        if path != SNOW_SHOT_CMAKE:
            failures.append(
                f"{path}:{number}: {FLAG} outside {SNOW_SHOT_CMAKE}; it would apply "
                "to targets that link only one Rust archive and hide real duplicate "
                "definitions")
    if len(found) > 1:
        failures.append(
            f"{FLAG} is written {len(found)} times; it belongs on exactly one "
            "INTERFACE target")

    # 2. Attached to `snow_shot_linux_platform`, as INTERFACE link options.
    attach_line = None
    for index, line in enumerate(lines):
        if "target_link_options(" not in line or "snow_shot_linux_platform" not in line:
            continue
        block = " ".join(part.strip() for part in lines[index : index + 4])
        if "INTERFACE" in block and FLAG in block:
            attach_line = index + 1
            break
    if attach_line is None:
        failures.append(
            f"{FLAG} is not attached with "
            f"`target_link_options(snow_shot_linux_platform INTERFACE ...)`; that "
            "target is what scopes it to the consumers of the second Rust archive")
    else:
        # 3. And that attachment is inside the Linux-only block.
        stack = guard_stack_at(lines, attach_line - 1)
        if not any(_platform_guard.search(condition) for condition in stack):
            failures.append(
                f"{SNOW_SHOT_CMAKE}:{attach_line}: {FLAG} is not inside an "
                "`if(UNIX AND NOT APPLE)` block")

    # 4. The archives the comment names still exist as crates.
    for archive in RUST_ARCHIVES:
        stem = archive[: -len(".a")][len("lib") :]
        package = stem.replace("_", "-")
        manifests = [
            path for path in walk_files(ROOT, ("Cargo.toml",))
            if re.search(r'^name\s*=\s*"' + re.escape(package) + r'"', 
                         path.read_text(encoding="utf-8", errors="replace"), re.M)
        ]
        if not manifests:
            failures.append(
                f"no Cargo.toml declares package '{package}', so {archive} can no "
                "longer be built and this gate is guarding a stale assumption")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"Rust runtime duplication check failed ({len(failures)} problem(s))",
              file=sys.stderr)
        return 1

    print(
        "Rust runtime duplication check passed "
        f"({len(RUST_ARCHIVES)} archives, {FLAG} attached once, "
        f"at {SNOW_SHOT_CMAKE}:{attach_line})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
