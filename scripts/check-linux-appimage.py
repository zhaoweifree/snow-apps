#!/usr/bin/env python3
"""Validate the AppImage bundle script without an AppImage host.

`appimagetool` only runs on Linux, and the failures that matter in AppRun are
the ones that survive every test that does not actually launch the bundle:

  * a CRLF line ending turns ``#!/bin/sh`` into ``bad interpreter: /bin/sh^M``
    and nothing runs at all. This repository is developed on Windows with
    ``core.autocrlf`` in play, so it is a live hazard, not a theoretical one;
  * a missing executable bit makes appimagetool emit an AppImage whose AppRun
    the runtime cannot execute;
  * a missing ``QT_PLUGIN_PATH`` export produces the "could not load the Qt
    platform plugin xcb" dialog, and that only shows up on a machine without
    the host's Qt -- i.e. never on the build host;
  * the ``exec`` line has to name the binary the install rules actually ship.

Usage: ``python scripts/check-linux-appimage.py`` (the shell entry point
``scripts/check-linux-packaging.sh`` runs this alongside the other gates).
"""

from __future__ import annotations

import ast
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
APPRUN = REPO / "snow_shot" / "packaging" / "linux" / "appimage" / "AppRun"
MODULE = REPO / "cmake" / "SnowShotAppImage.cmake"
ROOT_CMAKE = REPO / "CMakeLists.txt"
BUILD_CHECK = REPO / "scripts" / "check-linux-appimage-build.py"

failures: list[str] = []


def _path_chain(node: ast.AST):
    """Flatten `REPO / "a" / "b"` into ["REPO", "a", "b"], or None.

    Anything that is not a literal segment (a variable, an f-string) makes the
    whole chain unresolvable from here and returns None.
    """
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        left = _path_chain(node.left)
        right = _path_chain(node.right)
        if left is None or right is None:
            return None
        return left + right
    if isinstance(node, ast.Name) and node.id == "REPO":
        return ["REPO"]
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    return None


def repo_relative_chains(path: Path):
    """Yield (source text, resolved path) for every `REPO / "..."` chain in a file.

    Parsed with `ast` rather than with a regex on purpose: a regex matches the
    chain inside a comment just as happily as the real expression, and the
    comment that documents this very check produced a chain (`REPO / "..."`)
    that resolved to the repository itself on a host where trailing dots are
    normalised away. Only real expressions are interesting.
    """
    text = path.read_text(encoding="utf-8")
    tree = ast.parse(text)
    chains = {}
    for node in ast.walk(tree):
        if not (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div)):
            continue
        parts = _path_chain(node)
        if not parts or parts[0] != "REPO" or len(parts) < 2:
            continue
        chains.setdefault(tuple(parts[1:]), ast.get_source_segment(text, node) or "")
    # An outer chain contains its own left half; keep only the longest form.
    longest = []
    for parts in chains:
        if any(other != parts and other[:len(parts)] == parts for other in chains):
            continue
        longest.append(parts)
    for parts in longest:
        yield chains[parts].strip(), REPO.joinpath(*parts)


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def cmake_variable(path: Path, name: str) -> str | None:
    match = re.search(rf'set\(\s*{name}\s+"?([^")\n]+)"?\s*\)', path.read_text(encoding="utf-8"))
    return match.group(1).strip() if match else None


def main() -> int:
    for path in (APPRUN, MODULE, ROOT_CMAKE):
        if not path.exists():
            check(f"{path.relative_to(REPO)} exists", False, "missing")
    if failures:
        return 1

    raw = APPRUN.read_bytes()
    text = raw.decode("utf-8")

    # --- The shebang has to survive the checkout ------------------------------
    check("AppRun has no CRLF", b"\r\n" not in raw,
          "a CRLF shebang makes the kernel report 'bad interpreter: /bin/sh^M'")
    check("AppRun has no BOM", not raw.startswith(b"\xef\xbb\xbf"))
    check("AppRun starts with a POSIX shebang", text.startswith("#!/bin/sh\n"),
          text.splitlines()[0] if text else "<empty>")

    # --- The executable bit has to be in the index, not just on disk ----------
    # chmod on a Windows checkout does not reach the repository, so the mode git
    # will restore on a Linux clone is the only one that matters.
    try:
        staged = subprocess.run(
            ["git", "-C", str(REPO), "ls-files", "-s", "--", str(APPRUN.relative_to(REPO))],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        staged = ""
        print(f"note git index unavailable ({error}); skipping the mode check")
    if staged:
        check("AppRun is executable in the git index", staged.startswith("100755"),
              f"mode is {staged.split()[0]}")
    else:
        check("AppRun is executable on disk",
              (APPRUN.stat().st_mode & 0o111) != 0, "chmod +x snow_shot/packaging/linux/appimage/AppRun")

    # --- Syntax ---------------------------------------------------------------
    # `sh -n` parses without executing, which is what catches the quoting
    # mistakes that would only appear inside the end user's desktop session.
    for shell in ("sh", "bash"):
        try:
            parsed = subprocess.run([shell, "-n", str(APPRUN)], capture_output=True, text=True)
        except OSError:
            print(f"note {shell} not available; skipping the syntax check")
            continue
        check(f"AppRun parses under {shell} -n", parsed.returncode == 0,
              parsed.stderr.strip())

    # --- The exports that fix the classic bundle failures ---------------------
    # Each of these is one specific symptom, so they are named rather than
    # lumped into "the script sets some variables".
    for variable, symptom in (
        ("QT_PLUGIN_PATH", "Qt cannot find the xcb platform plugin"),
        ("LD_LIBRARY_PATH", "the bundled libraries are not on the loader's path"),
        ("XDG_DATA_DIRS", "the bundled icon and AppStream file are never found"),
    ):
        check(f"AppRun exports {variable}", re.search(rf"^export {variable}=", text, re.MULTILINE) is not None,
              symptom)
    # Only an assignment counts: the script explains in a comment why this
    # variable is not set, and that comment must not trip the check.
    check("AppRun does not set the deprecated QT_QPA_PLATFORM_PLUGIN_PATH",
          re.search(r"^\s*(export\s+)?QT_QPA_PLATFORM_PLUGIN_PATH=", text, re.MULTILINE) is None,
          "it overrides QT_PLUGIN_PATH and hides every other plugin")
    check("AppRun derives APPDIR when the runtime did not set it",
          "readlink -f" in text)

    # --- The exec line has to reach the shipped binary ------------------------
    executable = cmake_variable(ROOT_CMAKE, "SNOW_SHOT_EXECUTABLE_NAME") or "snow_shot"
    exec_lines = [line.strip() for line in text.splitlines() if line.strip().startswith("exec ")]
    check("AppRun execs exactly one process", len(exec_lines) == 1, str(exec_lines))
    if exec_lines:
        check("AppRun execs the installed binary",
              f"$APPDIR/usr/bin/{executable}" in exec_lines[0], exec_lines[0])

    # --- The packaging module and the entry agree on the icon -----------------
    module_text = MODULE.read_text(encoding="utf-8")
    check("module stages the AppRun", "packaging/linux/appimage/AppRun" in module_text)
    check("module passes ARCH to appimagetool", 'ARCH=${SNOW_SHOT_APPIMAGE_ARCH}' in module_text.replace('"', ""))
    check("module works without FUSE", "APPIMAGE_EXTRACT_AND_RUN=1" in module_text)
    check("module writes .DirIcon", ".DirIcon" in module_text)
    check("root CMakeLists gates the module behind an option",
          "SNOW_APPS_PACKAGE_APPIMAGE" in ROOT_CMAKE.read_text(encoding="utf-8"))

    # --- The runtime the AppImage embeds is supplied, not downloaded ----------
    # The maintained appimagetool (AppImage/appimagetool) does not bundle a
    # type-2 runtime: without `--runtime-file` it fetches one at packaging time,
    # which makes the artefact depend on what upstream served that minute. The
    # module has to be able to pass one through.
    check("module accepts a runtime file",
          "SNOW_APPS_APPIMAGETOOL_RUNTIME" in module_text,
          "the maintained appimagetool downloads a runtime unless one is supplied")
    check("module hands the runtime to appimagetool",
          "--runtime-file" in module_text,
          "the cache variable alone would never reach appimagetool")
    check("module fails loudly on a missing runtime",
          "SNOW_APPS_APPIMAGETOOL_RUNTIME does not exist" in module_text,
          "a bad path has to stop the packaging, not silently fall back")

    # The fetch script is what makes the two paths above exist on a clean
    # runner; it is a separate file because the pins have to live in one place.
    fetch = REPO / "scripts" / "fetch-appimagetool.sh"
    if check("scripts/fetch-appimagetool.sh exists", fetch.is_file(), "missing"):
        fetch_text = fetch.read_text(encoding="utf-8", errors="replace")
        # Both names also appear in the script's usage block, so the checks are
        # against the `echo` that actually writes them to stdout -- a gate that
        # matched the documentation would stay green while the emit was gone.
        check("fetch script emits SNOW_APPS_APPIMAGETOOL",
              'echo "SNOW_APPS_APPIMAGETOOL=${tool_path}"' in fetch_text,
              "the workflow appends its stdout to $GITHUB_ENV")
        check("fetch script emits SNOW_APPS_APPIMAGETOOL_RUNTIME",
              'echo "SNOW_APPS_APPIMAGETOOL_RUNTIME=${runtime_path}"' in fetch_text,
              "the runtime path is what the module passes to --runtime-file")
        # The word itself is fine in the comment that explains why the rolling
        # tag is avoided; what matters is that no assignment or URL uses it.
        code_lines = [
            line for line in fetch_text.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        check("fetch script pins a version rather than a rolling tag",
              not any("continuous" in line for line in code_lines),
              "assets under the `continuous` tag are replaced in place, so a "
              "checksum pinned to it expires without anyone editing the file")
        check("fetch script verifies a checksum per architecture",
              all(name in fetch_text for name in
                  ("APPIMAGETOOL_SHA256_x86_64", "APPIMAGETOOL_SHA256_aarch64",
                   "RUNTIME_SHA256_x86_64", "RUNTIME_SHA256_aarch64")),
              "a truncated or replaced download has to fail instead of shipping")

    # Every `REPO / "a" / "b"` chain in the product validator has to resolve.
    # That script runs on a CI runner holding a real AppImage, and nothing else
    # parses it first: when its reviewed-AppRun anchor pointed at the repository
    # root (no `packaging/` has ever lived there) the comparison silently
    # skipped, so the check reported success without comparing two files. This
    # resolves the literals against the working tree, which is the only place
    # the mistake is visible without a Linux package.
    for chain, resolved in repo_relative_chains(BUILD_CHECK):
        check(f"{BUILD_CHECK.name} resolves {chain}", resolved.exists(),
              f"{chain} -> {resolved}")

    # The product validator unpacks the file with `cwd` set to a temporary
    # directory, so the path it was handed has to be absolute before that
    # point. The runner hands it `build/<preset>/snow-shot-*.AppImage`, relative
    # to the repository root; run 44 unpacked from the temp directory and died
    # with `FileNotFoundError: … snow-shot-1.2.8-x86_64.AppImage` for a file
    # that existed at the time. Nothing on the workstation can execute a Linux
    # AppImage, so this pairing is the only place the hazard is visible here.
    build_text = BUILD_CHECK.read_text(encoding="utf-8", errors="replace")
    if check("the product validator runs the AppImage under a temporary cwd",
             re.search(r"cwd\s*=\s*(?:str\()?\s*workdir", build_text) is not None,
             "if this is no longer true the resolve below stops being required"):
        check("the product validator makes the AppImage path absolute first",
              "Path(args.appimage).resolve()" in build_text,
              "a relative path stops resolving the moment cwd changes")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nAppImage bundle check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
