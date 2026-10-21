#!/usr/bin/env python3
"""Keep the Linux dependency pins in sync with the vcpkg ports they came from.

`scripts/build-linux-deps.sh` builds a handful of libraries from source because
the distribution versions are too old or have no CMake config. For `zlib` and
`minizip-ng` the version and SHA512 are read out of the port at run time, so they
cannot drift. For the rest the pins are *copied* into the script: `imath`,
`openexr`, `libwebp` and `highway` come from vcpkg builtin ports (which live in
the `.tools/vcpkg` clone, absent on CI), while `libjxl` and `libheif` come from
this repository's overlay ports.

A copy is exactly the thing that goes stale: bump the port, and the Linux build
keeps compiling the old archive against a `snow_image` that expects the new API
(that is how the EXR/WebP failures of CI run 39602920361 happened, only with the
distribution's version instead of a stale pin). This check compares the copy
against the port, and also asserts the script's plumbing: every `build_<name>`
has a version+sha512 pin and appears in the build loop.

Overlay comparisons run anywhere; builtin ones need the clone. A pin that cannot
be compared prints a skip line -- but if *nothing* could be compared the check
fails, so a broken port layout cannot masquerade as a clean run.

Usage: python scripts/check-linux-deps-pins.py [--root .]
Exit: 0 clean or skipped, 1 drift or plumbing error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# script pin name -> candidate port directories, most authoritative first.
# Overlay ports live in the repository (a CI runner has them); builtin ports
# live in the `.tools/vcpkg` clone (a CI runner does not).
PORTS = {
    "imath": ["imath"],
    "openexr": ["openexr"],
    "libwebp": ["libwebp"],
    "highway": ["highway"],
    "libjxl": ["libjxl"],
    "libheif": ["libheif"],
}
# Pins that are read from the port at run time; nothing to compare.
RUNTIME_READ = {"zlib", "minizip-ng"}
# Pins with no vcpkg port at all: the archive URL is their own choice.
NO_PORT = {"libjpeg-turbo", "lunasvg", "onnxruntime"}

VERSION = re.compile(r'^([a-z0-9_]+)_version="([^"]*)"', re.MULTILINE)
SHA512 = re.compile(r'^([a-z0-9_]+)_sha512="([^"]*)"', re.MULTILINE)
BUILDER = re.compile(r"^build_([a-z0-9_]+)\(\)", re.MULTILINE)
LOOP = re.compile(r"^for library in (.+?); do", re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    script = root / "scripts" / "build-linux-deps.sh"
    if not script.is_file():
        print(f"missing {script}", file=sys.stderr)
        return 2
    text = script.read_text(encoding="utf-8")

    failures: list[str] = []

    versions = dict(VERSION.findall(text))
    checksums = dict(SHA512.findall(text))
    builders = {f"build_{name}" for name in BUILDER.findall(text)}
    loop = LOOP.search(text)
    if loop is None:
        print("could not find the build loop in build-linux-deps.sh", file=sys.stderr)
        return 2
    looped = loop.group(1).split()

    # Plumbing: every name in the loop has a builder, a version and a checksum
    # (onnxruntime's checksums are per-architecture and may still be empty on
    # purpose -- see its comment).
    for name in looped:
        if f"build_{name.replace('-', '_')}" not in builders:
            failures.append(f"loop lists '{name}' but no build_{name.replace('-', '_')}() exists")
        if name == "onnxruntime":
            # Per-architecture pins: Microsoft publishes no checksum, so these
            # were read off the assets the first real run downloaded. Empty
            # values would mean the archive is used unverified.
            for arch in ("x64", "aarch64"):
                value = re.search(rf'^onnxruntime_sha512_{arch}="([0-9a-f]{{128}})"',
                                  text, re.MULTILINE)
                if value is None:
                    failures.append(f"onnxruntime has no sha512 pin for {arch} "
                                    f"(the prebuilt archive would be used unverified)")
            continue
        if name in RUNTIME_READ:
            continue
        key = name.replace("-", "_")
        if key not in versions:
            failures.append(f"'{name}' has no {key}_version=\"...\" pin")
        if not checksums.get(key):
            failures.append(f"'{name}' has no {key}_sha512=\"...\" pin")
    looped_builders = {f"build_{name.replace('-', '_')}" for name in looped}
    for builder in sorted(builders - looped_builders):
        failures.append(f"{builder}() exists but is not in the build loop")

    # Provenance: compare the copied pins with the ports they were copied from.
    # Overlay ports are in the repository, so those comparisons always run;
    # builtin ports only exist on a workstation that has the vcpkg clone.
    port_roots = [root / "cmake" / "vcpkg-overlay-ports", root / ".tools" / "vcpkg" / "ports"]
    compared = 0
    for pin_name, candidates in sorted(PORTS.items()):
        port = next((base / name
                     for name in candidates
                     for base in port_roots
                     if (base / name / "vcpkg.json").is_file()), None)
        port_name = port.name if port is not None else "/".join(candidates)
        if port is None:
            print(f"skipped {pin_name}: no port directory on this machine "
                  f"(checked {', '.join(str(p.relative_to(root)) for p in port_roots)})")
            continue
        manifest = port / "vcpkg.json"
        portfile = port / "portfile.cmake"
        if not portfile.is_file():
            failures.append(f"port '{port_name}' has no portfile.cmake to compare against")
            continue
        manifest_json = json.loads(manifest.read_text(encoding="utf-8"))
        port_version = manifest_json.get("version") or manifest_json.get("version-semver")
        match = re.search(r"^\s*SHA512\s+([0-9a-f]{128})\s*$",
                          portfile.read_text(encoding="utf-8"), re.MULTILINE)
        if match is None:
            failures.append(f"port '{port_name}' has no SHA512 line to compare against")
            continue
        port_sha = match.group(1)
        if versions.get(pin_name) != port_version:
            failures.append(
                f"{pin_name}: script pins version {versions.get(pin_name)!r} "
                f"but port '{port_name}' is {port_version!r}")
        if checksums.get(pin_name) != port_sha:
            failures.append(
                f"{pin_name}: script pin sha512 differs from port '{port_name}' "
                f"(script {checksums.get(pin_name, '')[:16]}..., port {port_sha[:16]}...)")
        print(f"ok   {pin_name}: matches port '{port_name}' ({port_version}, "
              f"{port.relative_to(root).as_posix()})")
        compared += 1
    if compared == 0:
        failures.append("no pin could be compared against a port")

    # The three lists must not silently disagree about which libraries exist.
    # Pin variable names use underscores (`libjpeg_turbo_version`), the loop
    # uses hyphens (`libjpeg-turbo`), so compare on the hyphen form.
    classified = set(PORTS) | RUNTIME_READ | NO_PORT
    unknown = {name.replace("_", "-") for name in versions} - classified
    if unknown:
        failures.append(f"pins with no classification: {', '.join(sorted(unknown))} "
                        f"(add them to PORTS, RUNTIME_READ or NO_PORT)")

    for failure in failures:
        print(f"FAIL {failure}")

    print(f"linux dependency pins: {'OK' if not failures else f'{len(failures)} problem(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
