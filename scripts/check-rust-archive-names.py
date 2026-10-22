#!/usr/bin/env python3
"""The archive name CMake waits for has to be the archive name Cargo writes.

`snow_add_rust_static_library` does not build the library; it declares an
IMPORTED STATIC target whose IMPORTED_LOCATION is

    ${SNOW_RUST_CARGO_TARGET_DIR}/<triple>/<profile>/lib<OUTPUT_NAME>.a

and depends on a custom target that runs `cargo build`. Cargo names that file
after the crate's *library* name - `[lib] name` in the Cargo.toml, defaulting to
the package name with dashes turned into underscores - and nothing renames it
afterwards. So `OUTPUT_NAME` is not a label, it is a promise about a file, and
a promise that is never checked at configure time:

    /usr/bin/ld: cannot find cargo/aarch64-unknown-linux-gnu/release/
    libsnow_linux_platform.a: No such file or directory  (CI run 27)

`snow-linux-c` was registered with `OUTPUT_NAME snow_linux_platform`. Cargo had
in fact built it - the log shows `Compiling snow-linux-c` and the archive is
`libsnow_linux_c.a` - and 608 of 608 translation units had compiled. The only
thing that disagreed was a filename, and it took a link step on an architecture
nobody builds locally to find out.

The rule: for every Rust static library registration, `OUTPUT_NAME` must equal
the underscored library name that Cargo derives from the package's Cargo.toml.
A package that cannot be found is a failure, not a skip.

Run it directly; the Linux packaging gate runs it as well.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SKIP_DIRECTORIES = {
    ".git",
    ".tools",
    "artifacts",
    "build",
    "node_modules",
    "target",
}

CMAKE_SUFFIXES = (".cmake", ".txt")

# Arguments of the two registration functions. `STRIP_MSVC_DIRECTIVES` is an
# option (no value); everything else takes one value.
SINGLE_KEYWORDS = ("PACKAGE", "MANIFEST_DIR", "OUTPUT_NAME")
SINGLE_OPTIONS = ("STRIP_MSVC_DIRECTIVES",)
BATCH_KEYWORDS = ("MANIFEST_DIR", "TARGETS", "PACKAGES", "OUTPUT_NAMES", "FEATURES")
BATCH_OPTIONS = ("STRIP_MSVC_DIRECTIVES",)


def walk_files(root: Path, suffixes: tuple[str, ...]):
    for directory, subdirectories, filenames in os.walk(root):
        subdirectories[:] = [name for name in subdirectories if name not in SKIP_DIRECTORIES]
        for filename in filenames:
            if filename.endswith(suffixes):
                yield Path(directory) / filename


def call_arguments(text: str, name: str):
    """Yield the argument text of every `name(...)` call, parentheses balanced."""
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for match in pattern.finditer(text):
        depth = 0
        start = match.end() - 1
        index = start
        while index < len(text):
            character = text[index]
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    yield match.start(), text[start + 1 : index]
                    index = len(text)
                    break
            index += 1
        else:
            raise SystemExit(f"unbalanced parentheses in call to {name}")


def strip_comments(text: str) -> str:
    """Drop `#` comments so that commented-out calls are not parsed."""
    lines = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            lines.append("")
            continue
        lines.append(line)
    return "\n".join(lines)


def unquote(token: str) -> str:
    if len(token) >= 2 and token[0] == token[-1] and token[0] in ('"', "'"):
        return token[1:-1]
    return token


def split_values(tokens: list[str], keywords: tuple[str, ...], options: tuple[str, ...]):
    """Split a flat token list into keyword -> value tokens."""
    values: dict[str, list[str]] = {}
    current = None
    for token in tokens:
        if token in keywords or token in options:
            current = token
            values.setdefault(current, [])
            continue
        if current is None:
            # The batch name / target name is the first positional argument.
            continue
        values[current].append(unquote(token))
    return values


def first(values: dict[str, list[str]], key: str) -> str | None:
    entries = values.get(key) or []
    return entries[0] if entries else None


def cargo_library_names() -> dict[str, str]:
    """package name -> underscored library name, for the whole workspace."""
    names: dict[str, str] = {}
    for manifest in walk_files(ROOT, ("Cargo.toml",)):
        section = None
        package_name = None
        library_name = None
        for line in manifest.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                section = stripped.strip("[]")
                continue
            match = re.match(r'name\s*=\s*"([^"]+)"', stripped)
            if not match:
                continue
            if section == "package" and package_name is None:
                package_name = match.group(1)
            elif section == "lib" and library_name is None:
                library_name = match.group(1)
        if package_name is None:
            continue
        names[package_name] = (library_name or package_name).replace("-", "_")
    return names


def check() -> int:
    libraries = cargo_library_names()
    registrations: list[tuple[Path, int, str, str]] = []
    forwarding = 0
    problems: list[str] = []

    for cmake_file in sorted(walk_files(ROOT, CMAKE_SUFFIXES)):
        text = strip_comments(cmake_file.read_text(encoding="utf-8", errors="replace"))
        relative = cmake_file.relative_to(ROOT)

        for offset, arguments in call_arguments(text, "snow_add_rust_static_library"):
            values = split_values(arguments.split(), SINGLE_KEYWORDS, SINGLE_OPTIONS)
            package = first(values, "PACKAGE")
            output_name = first(values, "OUTPUT_NAME")
            if not package:
                problems.append(f"{relative}: a registration has no PACKAGE")
                continue
            if "$" in package:
                # A wrapper forwarding its own arguments; `snow_add_rust_static_library`
                # itself is the one such call. Its call sites are checked instead.
                forwarding += 1
                continue
            line = text.count("\n", 0, offset) + 1
            # `snow_add_rust_static_library` defaults OUTPUT_NAME to PACKAGE.
            registrations.append((relative, line, package, output_name or package))

        for offset, arguments in call_arguments(text, "snow_add_rust_static_libraries"):
            values = split_values(arguments.split(), BATCH_KEYWORDS, BATCH_OPTIONS)
            packages = values.get("PACKAGES") or []
            output_names = values.get("OUTPUT_NAMES") or []
            targets = values.get("TARGETS") or []
            if not packages:
                problems.append(f"{relative}: a batch registration has no PACKAGES")
                continue
            if any("$" in package for package in packages):
                forwarding += len(packages)
                continue
            if len(output_names) not in (0, len(packages)):
                problems.append(
                    f"{relative}: OUTPUT_NAMES has {len(output_names)} entries "
                    f"for {len(packages)} packages")
                continue
            line = text.count("\n", 0, offset) + 1
            for index, package in enumerate(packages):
                if index < len(output_names):
                    output_name = output_names[index]
                elif index < len(targets):
                    output_name = targets[index]
                else:
                    output_name = package
                registrations.append((relative, line, package, output_name))

    for relative, line, package, output_name in registrations:
        library_name = libraries.get(package)
        if library_name is None:
            problems.append(
                f"{relative}:{line}: package '{package}' has no Cargo.toml in this "
                f"workspace, so the archive name cannot be verified")
            continue
        if output_name.startswith("$"):
            problems.append(
                f"{relative}:{line}: OUTPUT_NAME '{output_name}' for '{package}' is a "
                f"variable; the archive name cannot be verified")
            continue
        expected = output_name.replace("-", "_")
        if expected != library_name:
            problems.append(
                f"{relative}:{line}: '{package}' registers OUTPUT_NAME '{output_name}', "
                f"which expects lib{expected}.a, but Cargo writes lib{library_name}.a "
                f"(package '{package}', [lib] name defaults to the package name)")

    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        print(f"Rust archive name check failed ({len(problems)} problem(s))", file=sys.stderr)
        return 1

    print(
        f"Rust archive name check passed "
        f"({len(registrations)} registration(s), {len(libraries)} crate(s) indexed, "
        f"{forwarding} forwarding argument(s) skipped)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(check())
