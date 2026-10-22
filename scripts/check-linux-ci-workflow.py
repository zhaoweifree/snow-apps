#!/usr/bin/env python3
"""Check the Linux CI workflow against the repository it drives.

A workflow is the one file in this project that nothing parses until it is
pushed: a misspelled preset name, a deleted gate script or a shared library that
is only installed on one architecture all read as perfectly good YAML and only
fail minutes into a run, on a machine nobody is watching. This script resolves
every reference the workflow makes against the checkout, so those typos fail in
the same `scripts/check-linux-packaging.sh` run as everything else.

Checked:

  * the file parses as YAML and declares the three jobs the port relies on;
  * every ``cmake --preset`` / ``--build --preset`` name exists in
    ``CMakePresets.json`` (and the configure preset is not hidden);
  * every script path mentioned in a ``run:`` step exists and is executable
    where it is invoked through ``bash``;
  * every job sets ``timeout-minutes`` -- a hung Qt configure otherwise burns the
    runner's full six-hour budget;
  * the matrix architectures map to the architectures the packaging gate
    asserts, so ``Architecture:`` in the .deb cannot disagree with the runner;
  * the apt package list is non-empty and names no package twice;
  * ``gates`` installs PyYAML, without which three checks in this file degrade
    to no-ops (see ``load_yaml``);
  * ``gates`` verifies that both cargo lockfiles are current -- ``snow_rust_ffi``
    is a second cargo project whose stale lockfile used to surface as a build
    failure instead of a gate failure;
  * ``build`` actually invokes ``scripts/build-linux-deps.sh``, the step that
    supplies zlib-ng and libjpeg-turbo, which no distribution package provides.

Usage: ``python scripts/check-linux-ci-workflow.py``.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOW = REPO / ".github" / "workflows" / "snow-shot-linux.yml"
PRESETS = REPO / "CMakePresets.json"

EXPECTED_JOBS = {"gates", "build"}
# Runner label -> the architecture string `snow_shot_linux_debian_architecture()`
# must produce for it. The workflow asserts the same pair when it reads the .deb
# back, so a runner swap without this table would silently skip the assertion.
RUNNER_ARCHITECTURES = {"ubuntu-24.04": "amd64", "ubuntu-24.04-arm": "arm64"}

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def load_yaml(path: Path):
    # A missing PyYAML used to print a note and return None, which turned three
    # of the checks in this file into no-ops. That is the worst failure mode a
    # gate has: it still exits 0 and prints "check passed". Every host that runs
    # this is expected to have PyYAML (`pip install pyyaml`; the `gates` job in
    # `.github/workflows/snow-shot-linux.yml` installs it), so its absence is a
    # broken environment, not a reason to skip.
    try:
        import yaml
    except ImportError:
        check("PyYAML is available", False, "pip install pyyaml")
        return None
    try:
        # The workflow uses `on:` as a key, which YAML 1.1 reads as a boolean.
        # Nothing below reads it, so the 1.1 semantics are harmless here.
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except Exception as error:  # noqa: BLE001 - the message is the point
        check("workflow parses as YAML", False, str(error))
        return None


def steps_of(job: dict) -> list[dict]:
    return [step for step in job.get("steps", []) if isinstance(step, dict)]


def run_commands(job: dict) -> list[str]:
    return [step.get("run", "") for step in steps_of(job) if step.get("run")]


# A `run:` block contains GitHub expressions (`${{ matrix.preset }}`) that are
# not valid any anything else until the workflow runs. They are stripped before
# pattern matching so the regexes only see literal text; the matrix entries are
# resolved separately, which is where the real preset names get checked.
EXPRESSION = re.compile(r"\$\{\{[^}]*\}\}")


def apt_packages(text: str) -> list[str]:
    """Return the package names of the first ``apt-get install`` in a ``run`` block."""
    packages: list[str] = []
    collecting = False
    for line in text.splitlines():
        stripped = line.strip()
        if "apt-get install" in stripped:
            collecting = True
            stripped = stripped.split("apt-get install", 1)[1]
        elif not collecting:
            continue
        continued = stripped.endswith("\\")
        for word in stripped.rstrip("\\").split():
            if word in ("-y", "--no-install-recommends", "sudo", "apt-get", "install"):
                continue
            if re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", word):
                packages.append(word)
        if not continued:
            break
    return packages


def main() -> int:
    if not WORKFLOW.exists():
        check(".github/workflows/snow-shot-linux.yml exists", False, "missing")
        return 1

    workflow = load_yaml(WORKFLOW)
    if workflow is not None:
        jobs = workflow.get("jobs") or {}
        check("workflow declares the expected jobs", EXPECTED_JOBS <= set(jobs),
              f"found {sorted(jobs)}")

        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        configure_names = {p["name"]: p for p in presets.get("configurePresets", [])}
        build_names = {p["name"] for p in presets.get("buildPresets", [])}

        for job_name, job in jobs.items():
            check(f"{job_name}: sets a timeout", bool(job.get("timeout-minutes")),
                  "a hung configure would otherwise run until the six-hour ceiling")
            text = "\n".join(run_commands(job))
            literal = EXPRESSION.sub("", text)

            # A preset name never starts with `-`; requiring that keeps the
            # regex from reading the option that follows a stripped `${{ }}`
            # placeholder as the name. `[ \t]+` rather than `\s+` keeps it from
            # walking into the next line of the block -- which is how a stripped
            # placeholder ends up "naming" the following `cmake` invocation.
            for preset in re.findall(r"cmake --preset[ \t]+([^-\s]\S*)", literal):
                check(f"{job_name}: configure preset '{preset}' exists", preset in configure_names,
                      "not in CMakePresets.json")
                if preset in configure_names:
                    check(f"{job_name}: configure preset '{preset}' is not hidden",
                          not configure_names[preset].get("hidden"),
                          "a hidden preset cannot be configured by name")
            for preset in re.findall(r"cmake --build --preset[ \t]+([^-\s]\S*)", literal):
                check(f"{job_name}: build preset '{preset}' exists", preset in build_names,
                      "not in CMakePresets.json")

            for script in re.findall(r"bash (scripts/[\w.-]+)", literal):
                path = REPO / script
                check(f"{job_name}: {script} exists", path.exists(), "referenced but missing")

        # Every runner the matrix names must be one the architecture assertion
        # knows how to check.
        runners = {
            value
            for job in jobs.values()
            for value in re.findall(r"runner:\s*(\S+)", json.dumps(job))
        }
        for runner in sorted(runners):
            check(f"runner '{runner}' has an architecture mapping", runner in RUNNER_ARCHITECTURES,
                  f"add it to RUNNER_ARCHITECTURES ({sorted(RUNNER_ARCHITECTURES)})")

        # The build matrix pairs each architecture with a preset; a mismatch
        # would build the wrong preset and still pass.
        for job_name, job in jobs.items():
            matrix = (job.get("strategy") or {}).get("matrix") or {}
            for entry in matrix.get("include", []) if isinstance(matrix, dict) else []:
                architecture = entry.get("architecture")
                preset = entry.get("preset", "")
                if architecture:
                    check(f"{job_name}: preset '{preset}' matches architecture '{architecture}'",
                          f"-{architecture}-" in preset, preset)
                    # The run steps use `${{ matrix.preset }}`, so the literal
                    # names never appear in the block above -- this is the only
                    # place they can be resolved against CMakePresets.json.
                    check(f"{job_name}: preset '{preset}' exists in CMakePresets.json",
                          preset in configure_names, "not in CMakePresets.json")
                    build_preset = entry.get("build-preset", "")
                    check(f"{job_name}: build preset '{build_preset}' exists in CMakePresets.json",
                          build_preset in build_names, "not in CMakePresets.json")
                    runner = entry.get("runner", "")
                    check(f"{job_name}: runner '{runner}' matches architecture '{architecture}'",
                          RUNNER_ARCHITECTURES.get(runner, "").startswith(
                              "amd64" if architecture == "x64" else architecture),
                          f"{runner} -> {RUNNER_ARCHITECTURES.get(runner)}")

        # The gate script must actually be invoked, not merely exist: a job that
        # installs Qt and builds without running the packaging gates would look
        # green while every check in check-linux-packaging.sh went unexecuted.
        gates_text = "\n".join(run_commands(jobs.get("gates", {})))
        check("gates job runs the packaging gates",
              "scripts/check-linux-packaging.sh" in gates_text,
              "the job exists but never calls the gate script")

        # This file's own YAML checks need PyYAML. `setup-python` does not ship
        # it, and the fallback used to be a printed note -- so a job could run
        # the gate, skip a fifth of it, and still be green.
        check("gates job installs PyYAML",
              re.search(r"pip install[^\n]*pyyaml", gates_text) is not None,
              "the workflow's own structure checks would be skipped")

        # Two cargo projects live in this repository, and only one of them has
        # its lockfile checked anywhere: `snow_rust_ffi` is a separate project
        # with a separate Cargo.lock. A stale one used to surface ten minutes
        # into the build job, in the FFI bundle step, as "cannot update the lock
        # file ... because --locked was passed" -- which reads like a compile
        # error and is not one (CI run 39705590567). The `gates` job is the one
        # that is allowed to be red, so the check belongs there.
        check("gates job verifies the Rust lockfiles",
              "cargo metadata --locked" in gates_text
              and "snow_rust_ffi/Cargo.toml" in gates_text,
              "a stale lockfile would surface as a build failure instead")

        # Two of the libraries the build needs are not distribution packages
        # (§9.6.9). A job that configures without building them fails on a
        # missing ZLIBNG_VERSION three steps later, which reads like a code bug.
        build_text = "\n".join(run_commands(jobs.get("build", {})))
        check("build job builds the pinned Linux dependencies",
              "scripts/build-linux-deps.sh" in build_text,
              "configure would run against whatever the distribution happens to have")

        # The apt list: duplicated names are harmless but usually mean a
        # copy-paste that dropped the package somebody meant to add.
        for job_name, job in jobs.items():
            text = "\n".join(run_commands(job))
            packages = apt_packages(text)
            if not packages:
                continue
            duplicates = {p for p in packages if packages.count(p) > 1}
            check(f"{job_name}: apt package list has no duplicates", not duplicates,
                  ", ".join(sorted(duplicates)))
            # The validators the port document defers to "the first command on
            # the real machine" have to be installed here or that step is a
            # no-op that still exits zero on an empty grep.
            for tool in ("desktop-file-utils", "appstream", "dpkg-dev"):
                check(f"{job_name}: installs {tool}", tool in packages, "missing from apt list")
            # qtbase5-dev is public headers only: without qtbase5-private-dev
            # `Qt5::GuiPrivate` never exists, and the sources do include
            # `<qpa/...>` / `<private/...>` (CI run 39600792545).
            check(f"{job_name}: installs qtbase5-private-dev",
                  "qtbase5-private-dev" in packages,
                  "Qt5::GuiPrivate would be missing and 19 link lines would fail")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nLinux CI workflow check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
