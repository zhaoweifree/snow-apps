#!/usr/bin/env python3
"""Gate: the Rust-target probe error message must name the right remedy.

cmake/RustStaticLibrary.cmake probes the Rust toolchain with
`cargo metadata --filter-platform <target> --no-deps`. When that fails the
message has to say what to do about it *on the platform it is on*, and it has
to carry cargo's own output.

In CI run 39715098658 the only useful line was the one the probe used to
throw away with ERROR_QUIET. The message that replaced it blamed the target
(which was fine -- the toolchain install had been rolled back) and pointed at
scripts/bootstrap.ps1, a Windows script that does not exist on the Linux
runners.

CMake cannot catch a message(FATAL_ERROR ...) in process, so the error text
has to leave the process: scripts/check-rust-target-error.cmake runs the
extracted block and dies with the message on stderr. This driver runs it once
per platform and asserts on what it printed. The block itself is extracted
from the module and executed -- the fixture never keeps a copy of the message,
because a fixture that tests its own copy tests nothing.

Run: python scripts/check-rust-target-error.py
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURE = REPO / "scripts" / "check-rust-target-error.cmake"


def run_fixture(platform: str, target: str, fake_sh: bool = False) -> tuple[int, str]:
    """Run the CMake fixture for one platform; return (exit code, output)."""
    cmake = os.environ.get("CMAKE_COMMAND") or _find_cmake()
    command = [
        cmake,
        f"-DSNOW_FIXTURE_PLATFORM={platform}",
        f"-DSNOW_FIXTURE_TARGET={target}",
    ]
    if fake_sh:
        # Exercise the Unix fake-cargo branch even on a Windows host: that is
        # the branch the Linux CI runners take, and it must not be the one
        # thing in this gate that only ever runs in CI.
        command.append("-DSNOW_FIXTURE_FAKE_SH=1")
    command += ["-P", str(FIXTURE)]
    result = subprocess.run(
        command,
        cwd=str(REPO),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return result.returncode, result.stdout + result.stderr


def _find_cmake() -> str:
    """Locate a cmake executable: the env var, the PATH, or the known install."""
    override = os.environ.get("SNOW_CMAKE")
    if override:
        return override
    for name in ("cmake", "cmake.exe"):
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            candidate = Path(directory) / name
            if candidate.is_file():
                return str(candidate)
    known = Path(r"C:\snow-tools\cmake\cmake-4.2.3-windows-x86_64\bin\cmake.exe")
    if known.is_file():
        return str(known)
    raise SystemExit("no cmake found; set CMAKE_COMMAND or SNOW_CMAKE")


def _squash(text: str) -> str:
    """Collapse every run of whitespace into a single space.

    CMake's own error reporter wraps long messages at ~80 columns, so the
    remedy sentence comes back broken across lines ("...--target\\n
    x86_64-unknown-linux-gnu"). Asserting on the raw text would then be
    asserting on the reporter's line width, not on the message. Whitespace is
    not semantically meaningful in a command, so it is folded away first --
    a ';' is not whitespace and survives, which is exactly what the
    list-separator check needs.
    """
    return " ".join(text.split())


def main() -> int:
    failures: list[str] = []

    def check(label: str, ok: bool, detail: str) -> None:
        if ok:
            print(f"ok   {label}")
        else:
            print(f"FAIL {label}")
            print(f"     {detail}")
            failures.append(label)

    # --- Linux -------------------------------------------------------------
    code, linux = run_fixture("linux", "x86_64-unknown-linux-gnu")
    check(
        "the linux fixture aborted with a message",
        code != 0,
        f"the fixture exited 0, so the block under test never ran:\n{linux}",
    )
    check(
        "linux message names the rustup command",
        "rustup toolchain install" in linux,
        f"the Linux remedy is missing:\n{linux}",
    )
    check(
        "linux message does not point at bootstrap.ps1",
        "bootstrap.ps1" not in linux,
        f"a Windows script cannot help on Linux:\n{linux}",
    )
    check(
        "linux message carries cargo's output",
        "cargo said:" in linux,
        f"the cargo output was discarded again:\n{linux}",
    )
    check(
        "linux message carries cargo's stderr text",
        "failed to run custom build command" in linux,
        f"cargo's own words are missing:\n{linux}",
    )
    check(
        "linux message names the target",
        "x86_64-unknown-linux-gnu" in linux,
        f"the message must say which target:\n{linux}",
    )
    check(
        "linux message carries the full rustup --target flag",
        "--target x86_64-unknown-linux-gnu" in _squash(linux),
        f"the remedy must be copy-pasteable, with the target spelled out:\n{linux}",
    )
    check(
        "linux message has no list separator in the remedy",
        ";--target" not in _squash(linux) and "minimal ;" not in _squash(linux),
        f"set() with two values builds a list, so the ';' leaks into the "
        f"message; use string(CONCAT) instead:\n{linux}",
    )

    # --- Windows -----------------------------------------------------------
    code, windows = run_fixture("windows", "x86_64-pc-windows-msvc")
    check(
        "the windows fixture aborted with a message",
        code != 0,
        f"the fixture exited 0, so the block under test never ran:\n{windows}",
    )
    check(
        "windows message keeps the bootstrap.ps1 hint",
        "bootstrap.ps1" in windows,
        f"the Windows remedy was lost:\n{windows}",
    )
    check(
        "windows message does not suggest the rustup command",
        "rustup toolchain install" not in windows,
        f"the Linux remedy does not apply:\n{windows}",
    )
    check(
        "windows message carries cargo's output",
        "cargo said:" in windows,
        f"the cargo output was discarded again:\n{windows}",
    )
    check(
        "windows message names the target",
        "x86_64-pc-windows-msvc" in windows,
        f"the message must say which target:\n{windows}",
    )

    # --- the Unix fake cargo ----------------------------------------------
    # On the Linux CI runners the fake cargo is a .sh script, not a .bat.
    # That branch has to work too, so it is exercised explicitly.
    code, unix = run_fixture("linux", "x86_64-unknown-linux-gnu", fake_sh=True)
    check(
        "the unix fake cargo branch aborted with a message",
        code != 0,
        f"the fixture exited 0, so the block under test never ran:\n{unix}",
    )
    check(
        "the unix fake cargo branch carries cargo's stderr text",
        "failed to run custom build command" in unix,
        f"cargo's own words are missing on the .sh branch:\n{unix}",
    )
    check(
        "the unix fake cargo branch names the rustup command",
        "rustup toolchain install" in unix,
        f"the Linux remedy is missing on the .sh branch:\n{unix}",
    )
    check(
        "the unix fake cargo branch has no list separator in the remedy",
        ";--target" not in _squash(unix) and "minimal ;" not in _squash(unix),
        f"the list separator leaks into the message on the .sh branch too:\n{unix}",
    )

    print()
    if failures:
        print(f"FAILED: {len(failures)} problem(s)")
        return 1
    print("rust target error message check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
