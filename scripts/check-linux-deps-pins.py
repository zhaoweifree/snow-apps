#!/usr/bin/env python3
"""Keep the Linux dependency pins in sync with the vcpkg ports they came from.

`scripts/build-linux-deps.sh` builds a handful of libraries from source because
the distribution versions are too old or have no CMake config. For `zlib` and
`minizip-ng` the version and SHA512 are read out of the port at run time, so they
cannot drift. For the rest the pins are *copied* into the script: `imath`,
`openexr`, `libwebp` and `highway` come from vcpkg builtin ports (which live in
the `.tools/vcpkg` clone, absent on CI), while `libjxl`, `libheif` and `opencv`
come from this repository's overlay ports.

A copy is exactly the thing that goes stale: bump the port, and the Linux build
keeps compiling the old archive against a `snow_image` that expects the new API
(that is how the EXR/WebP failures of CI run 39602920361 happened, only with the
distribution's version instead of a stale pin). This check compares the copy
against the port, and also asserts the script's plumbing: every `build_<name>`
has a version+sha512 pin and appears in the build loop.

`opencv` needs two more things this file checks, because "the same source" is
not only about the archive:

  * its port fetches **two** archives (the main repository and
    `opencv_contrib`, which carries the `wechat_qrcode` module) *and* a patch
    file, so "the first SHA512 in the portfile" is the patch's checksum. The
    pins are compared per archive, keyed by the repository the
    `vcpkg_from_github` call names (`ARCHIVE_PINS`);
  * the `WeChatQRCode` in-memory-model constructor the Linux build relies on
    does not exist upstream -- it is added by a patch in this repository. The
    Linux build must apply *that* file, and the vcpkg port must apply the same
    one, or the two platforms compile different code against the same API.

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
    "opencv": ["opencv4"],
}
# Pins that are read from the port at run time; nothing to compare.
RUNTIME_READ = {"zlib", "minizip-ng"}
# Pins with no vcpkg port at all: the archive URL is their own choice.
NO_PORT = {"libjpeg-turbo", "lunasvg", "onnxruntime"}

# Ports that fetch more than one archive, and therefore cannot be compared
# with the "first SHA512 in the portfile" rule: the opencv4 port downloads a
# patch file first, and the main repository and opencv_contrib afterwards,
# each with its own checksum. The pins are compared per archive instead,
# keyed by the repository the `vcpkg_from_github` call names.
#   pin name -> {vcpkg REPO: (version pin name, sha512 pin name)}
# The names are the *pin prefixes* -- the script writes them as
# `<prefix>_version="..."` and `<prefix>_sha512="..."`.
ARCHIVE_PINS = {
    "opencv": {
        "opencv/opencv": ("opencv", "opencv"),
        "opencv/opencv_contrib": ("opencv_contrib", "opencv_contrib"),
    },
}

# The patch that adds the in-memory-model `WeChatQRCode` constructor. Both
# builds -- vcpkg and the Linux script -- have to apply this exact file: it
# is not an upstream API, so a build without it has a *different* constructor
# set and the barcode service stops compiling.
OPENCV_PATCH = "0024-contrib-wechat-qrcode-in-memory-models.patch"

VERSION = re.compile(r'^([a-z0-9_]+)_version="([^"]*)"', re.MULTILINE)
SHA512 = re.compile(r'^([a-z0-9_]+)_sha512="([^"]*)"', re.MULTILINE)
BUILDER = re.compile(r"^build_([a-z0-9_]+)\(\)", re.MULTILINE)
LOOP = re.compile(r"^for library in (.+?); do", re.MULTILINE)
# The WeChat QR models: `<name>|<sha512>|<md5>` per line inside the script's
# `wechat_qrcode_models` string, and `<name>|<sha512>` inside the project's
# own CMake downloader.
SCRIPT_MODEL_BLOCK = re.compile(r'wechat_qrcode_models="([^"]*)"', re.MULTILINE)
CMAKE_MODEL = re.compile(r'"([\w.]+)\|([0-9a-f]{128})"')


def from_github_checksums(portfile_text: str) -> dict[str, str]:
    """Map every `vcpkg_from_github` REPO to the SHA512 of that call.

    The rule has to be per call rather than "the first SHA512 in the file":
    the opencv4 port downloads a patch file (with its own SHA512) before it
    downloads the two source archives.
    """
    result: dict[str, str] = {}
    for match in re.finditer(r"vcpkg_from_github\s*\(", portfile_text):
        start = match.end()
        depth = 1
        index = start
        while index < len(portfile_text) and depth > 0:
            char = portfile_text[index]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1
        block = portfile_text[start:index - 1]
        repo = re.search(r"^\s*REPO\s+(\S+)\s*$", block, re.MULTILINE)
        sha = re.search(r"^\s*SHA512\s+([0-9a-f]{128})\s*$", block, re.MULTILINE)
        if repo and sha:
            result[repo.group(1)] = sha.group(1)
    return result



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

    # The per-archive pins (a port that fetches more than one source tree)
    # have to exist as variables as well, or the comparison below would only
    # ever report "None differs from the port".
    for pin_name, specs in sorted(ARCHIVE_PINS.items()):
        for repo, (version_pin, sha_pin) in sorted(specs.items()):
            if version_pin not in versions:
                failures.append(f"'{pin_name}' ({repo}) has no {version_pin}_version=\"...\" pin")
            if not checksums.get(sha_pin):
                failures.append(f"'{pin_name}' ({repo}) has no {sha_pin}_sha512=\"...\" pin")

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
        portfile_text = portfile.read_text(encoding="utf-8")

        archive_pins = ARCHIVE_PINS.get(pin_name)
        if archive_pins is not None:
            # Per-archive comparison: the pin is matched against the SHA512 of
            # the `vcpkg_from_github` call that names that repository.
            by_repo = from_github_checksums(portfile_text)
            for repo, (version_pin, sha_pin) in sorted(archive_pins.items()):
                port_sha = by_repo.get(repo)
                if port_sha is None:
                    failures.append(
                        f"port '{port_name}' has no SHA512 for {repo} to compare against")
                    continue
                if versions.get(version_pin) != port_version:
                    failures.append(
                        f"{pin_name}: script pins {version_pin}_version="
                        f"{versions.get(version_pin)!r} but port '{port_name}' is {port_version!r}")
                if checksums.get(sha_pin) != port_sha:
                    failures.append(
                        f"{pin_name}: script pin {sha_pin}_sha512 differs from port "
                        f"'{port_name}' ({repo}) (script {checksums.get(sha_pin, '')[:16]}..., "
                        f"port {port_sha[:16]}...)")
            print(f"ok   {pin_name}: matches port '{port_name}' ({port_version}, "
                  f"{port.relative_to(root).as_posix()}, {len(archive_pins)} archives)")
            compared += 1
            continue

        match = re.search(r"^\s*SHA512\s+([0-9a-f]{128})\s*$", portfile_text, re.MULTILINE)
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

    # --- the OpenCV in-memory-model patch ----------------------------------
    # The constructor the barcode service uses is not an upstream API: it is
    # added by a patch in this repository. Two builds that do not apply the
    # same patch do not compile the same code -- the Linux one would fail to
    # compile at all, which is the failure this whole exercise exists to
    # prevent. So the file the Linux script applies, the file the vcpkg port
    # applies, and the file in the repository have to be one and the same.
    patch_path = f"cmake/vcpkg-overlay-ports/opencv4/{OPENCV_PATCH}"
    if not (root / patch_path).is_file():
        failures.append(f"the OpenCV in-memory-model patch is missing: {patch_path}")
    if patch_path not in text:
        failures.append(
            f"build-linux-deps.sh does not reference {patch_path} -- the Linux build "
            f"would use the upstream WeChatQRCode, which has no in-memory constructor")
    opencv_port = root / "cmake" / "vcpkg-overlay-ports" / "opencv4" / "portfile.cmake"
    if opencv_port.is_file():
        port_text = opencv_port.read_text(encoding="utf-8")
        if OPENCV_PATCH not in port_text:
            failures.append(
                f"the opencv4 vcpkg port does not apply {OPENCV_PATCH} -- the Windows "
                f"build would use the upstream WeChatQRCode")
    else:
        failures.append("the opencv4 vcpkg port has no portfile.cmake to compare against")

    # --- the WeChat QR models ----------------------------------------------
    # The same four models, with the same checksums, on both sides: a model
    # that differs is a barcode that decodes on Windows and not on Linux.
    cmake_models_file = root / "cmake" / "FetchSnowShotQrModels.cmake"
    if not cmake_models_file.is_file():
        failures.append("cmake/FetchSnowShotQrModels.cmake is missing")
    else:
        cmake_models = dict(CMAKE_MODEL.findall(cmake_models_file.read_text(encoding="utf-8")))
        script_models: dict[str, str] = {}
        block = SCRIPT_MODEL_BLOCK.search(text)
        if block is None:
            failures.append("build-linux-deps.sh pins no WeChat QR model")
        else:
            for line in block.group(1).splitlines():
                line = line.strip()
                if not line:
                    continue
                parts = line.split("|")
                if len(parts) != 3 or not re.fullmatch(r"[0-9a-f]{128}", parts[1]) \
                        or not re.fullmatch(r"[0-9a-f]{32}", parts[2]):
                    failures.append(
                        f"the QR model pin {line!r} is not <name>|<sha512>|<md5>")
                    continue
                script_models[parts[0]] = parts[1]
        for name, sha in sorted(cmake_models.items()):
            if name not in script_models:
                failures.append(f"build-linux-deps.sh does not pin the QR model '{name}'")
            elif script_models[name] != sha:
                failures.append(
                    f"QR model '{name}': the script's sha512 differs from "
                    f"FetchSnowShotQrModels.cmake")
        for name in sorted(set(script_models) - set(cmake_models)):
            failures.append(
                f"the script pins the QR model '{name}', which "
                f"FetchSnowShotQrModels.cmake does not fetch")
        if cmake_models and script_models:
            print(f"ok   wechat qr models: {len(script_models)} models match "
                  f"FetchSnowShotQrModels.cmake")

    # The three lists must not silently disagree about which libraries exist.
    # Pin variable names use underscores (`libjpeg_turbo_version`), the loop
    # uses hyphens (`libjpeg-turbo`), so compare on the hyphen form.
    classified = set(PORTS) | RUNTIME_READ | NO_PORT
    for specs in ARCHIVE_PINS.values():
        for version_pin, _ in specs.values():
            classified.add(version_pin.replace("_", "-"))
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
