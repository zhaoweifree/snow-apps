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
  * the apt package lists are non-empty, name no package twice, and install the
    tools each job actually invokes -- the compiler headers where a job
    compiles, the two metadata validators where a job validates; a job is
    never asked to install a toolchain it does not use;
  * ``gates`` installs PyYAML, without which three checks in this file degrade
    to no-ops (see ``load_yaml``);
  * ``gates`` verifies that both cargo lockfiles are current -- ``snow_rust_ffi``
    is a second cargo project whose stale lockfile used to surface as a build
    failure instead of a gate failure;
  * ``gates`` asserts the AppRun is executable, because the AppDir copies its
    mode from the index and appimagetool only notices after the whole build;
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


def matrix_entries(job: dict) -> list[dict]:
    """The expanded matrix rows, i.e. what each job instance is parameterised by.

    Only `include:` is used here; a plain `axis: [...]` matrix has no
    architecture field to read, and the AppImage checks would then have nothing
    to compare against.
    """
    matrix = (job.get("strategy") or {}).get("matrix") or {}
    include = matrix.get("include")
    if not isinstance(include, list):
        return []
    return [entry for entry in include if isinstance(entry, dict)]


def run_commands(job: dict) -> list[str]:
    return [step.get("run", "") for step in steps_of(job) if step.get("run")]


# A `run:` block contains GitHub expressions (`${{ matrix.preset }}`) that are
# not valid any anything else until the workflow runs. They are stripped before
# pattern matching so the regexes only see literal text; the matrix entries are
# resolved separately, which is where the real preset names get checked.
EXPRESSION = re.compile(r"\$\{\{[^}]*\}\}")


def apt_packages(text: str) -> list[str]:
    """Every package named by any ``apt-get install`` in a job's ``run`` blocks.

    *Any*, not the first: this used to stop at the first block, so a job that
    installed its compiler in one step and a validator in a later step only ever
    had the first list checked -- and a package silently dropped from the second
    block read as "installed". A job is free to spread its packages over several
    steps; the assertion is about what the job ends up with.
    """
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
        # A line that does not end in `\` closes the block; keep scanning from
        # the next line instead of giving up on the rest of the job.
        if not continued:
            collecting = False
    return packages


# Matrix architecture -> the ELF machine name `check-linux-appimage-build.py`
# expects for it. The workflow's own `--expect-arch` expression has to name these
# two strings; a rename on either side would otherwise only show up as a wrong
# "architecture mismatch" report from a machine nobody is watching.
APPIMAGE_ARCHITECTURES = {"x64": "x86_64", "arm64": "aarch64"}

# Documentation artefacts that must not start a run. The port document is
# rendered to a root-level `.html`, and only `*.md` was ignored: a docs-only push
# started a run and `cancel-in-progress` killed the build being watched (run 25,
# cancelled by run 26).
IGNORED_DOCUMENTATION_PATTERNS = ("*.md", "*.html")


def paths_ignore_checks(workflow: dict) -> None:
    triggers = workflow.get("on") or workflow.get(True) or {}
    if not isinstance(triggers, dict):
        check("the workflow declares its triggers", False, f"found {type(triggers).__name__}")
        return
    for event in ("push", "pull_request"):
        config = triggers.get(event)
        if not isinstance(config, dict):
            check(f"{event}: declares paths-ignore", False, "no configuration")
            continue
        patterns = config.get("paths-ignore") or []
        missing = [pattern for pattern in IGNORED_DOCUMENTATION_PATTERNS
                   if pattern not in patterns]
        check(f"{event}: ignores the documentation artefacts", not missing,
              "missing " + ", ".join(missing) + " -- a docs-only push would start a "
              "full build and, with cancel-in-progress, kill the run being watched")


def appimage_checks(jobs: dict) -> None:
    """The AppImage packaging path has to stay wired, in this order.

    Order is the assertion, not decoration: the .deb is the artefact a
    distribution install uses, so it must be built and uploaded before a
    third-party tool from the network enters the job. If appimagetool's pins
    rot, the release still produced something installable that run.
    """
    job = jobs.get("build")
    if job is None:
        check("build job exists for the AppImage checks", False, "no build job")
        return

    steps = steps_of(job)
    names = [step.get("name", "") for step in steps]
    required = [
        "Upload the package",
        "Fetch appimagetool",
        "Build the AppImage",
        "Validate the AppImage",
        "Upload the AppImage",
    ]
    missing = [name for name in required if name not in names]
    check("build job keeps every AppImage step", not missing,
          "missing: " + ", ".join(missing))
    if missing:
        return

    positions = [names.index(name) for name in required]
    check("the .deb is packaged and uploaded before appimagetool is fetched",
          positions == sorted(positions),
          "order is: " + " -> ".join(required))

    by_name = {step.get("name", ""): step for step in steps}
    fetch = by_name["Fetch appimagetool"].get("run", "")
    check("Fetch appimagetool writes the pinned paths into the job environment",
          "fetch-appimagetool.sh" in fetch and "GITHUB_ENV" in fetch,
          "the next step reads SNOW_APPS_APPIMAGETOOL* from the environment")

    # Both options are mandatory and the script has no defaults for either: a
    # bare call dies with `error: --arch is required`, which is exactly what the
    # first run that reached this step reported (the checks here only asserted
    # the script name and `$GITHUB_ENV`, so a call with no arguments passed).
    for option in ("--arch", "--dest"):
        check(f"Fetch appimagetool passes {option}", option in fetch,
              "the script refuses to guess either value")

    pack = by_name["Build the AppImage"].get("run", "")
    for token in (
        "-DSNOW_APPS_PACKAGE_APPIMAGE=ON",
        '-DSNOW_APPS_APPIMAGETOOL="$SNOW_APPS_APPIMAGETOOL"',
        '-DSNOW_APPS_APPIMAGETOOL_RUNTIME="$SNOW_APPS_APPIMAGETOOL_RUNTIME"',
        "--target snow-shot-appimage",
    ):
        check(f"Build the AppImage passes {token}", token in pack,
              "the target is attached by the option, and the runtime has to be "
              "handed over or appimagetool downloads one at packaging time")

    # `cmake --install` walks every install rule, so the AppDir is empty without
    # the packaging configuration the .deb step turned on.
    check("Build the AppImage keeps the install rules enabled",
          "-DSNOW_APPS_PACKAGE_SNOW_SHOT=ON" in pack,
          "the AppDir is staged with cmake --install")

    validate = by_name["Validate the AppImage"].get("run", "")
    check("Validate the AppImage runs the product check",
          "scripts/check-linux-appimage-build.py" in validate,
          "an AppImage that is never unpacked is only a file that exists")

    # The workflow names its artefacts `x64`/`arm64`; appimagetool and the
    # runtime releases say `x86_64`/`aarch64`. The mapping lives in the matrix
    # now, so both steps have to read it from there -- and the matrix has to
    # actually carry it, or `--arch ""` reaches the script.
    declared = {entry.get("architecture"): entry.get("appimage-arch")
                for entry in matrix_entries(job)}
    check("the build matrix declares appimage-arch for every architecture",
          declared == APPIMAGE_ARCHITECTURES,
          f"declared {declared}, expected {APPIMAGE_ARCHITECTURES}")
    for label, text in (("Fetch appimagetool", fetch),
                        ("Validate the AppImage", validate)):
        check(f"{label} takes the ELF machine name from the matrix",
              "matrix.appimage-arch" in text,
              "x64/arm64 is the workflow's spelling, not the ELF machine name")

    root_cmake = REPO / "CMakeLists.txt"
    check("the root CMakeLists offers SNOW_APPS_PACKAGE_APPIMAGE",
          "SNOW_APPS_PACKAGE_APPIMAGE" in root_cmake.read_text(encoding="utf-8"),
          "the option the workflow turns on does not exist")


def metadata_validator_checks(jobs: dict) -> None:
    """Both metadata validators run in both jobs that can afford them.

    The apt check above only asks "the validator's package is installed *if*
    the job runs the validator". That is a check that disappears together with
    the thing it guards: delete the step and it goes vacuously green. Run 41
    is the reason both steps exist -- `appstreamcli` rejected the metainfo for
    a missing release date that neither `desktop-file-validate` nor the
    repository's own structural gate can see, and it did so after the whole
    build had finished:

        E: snow-shot.desktop:37: release-time-missing date

    `gates` can run them in seconds (no compiler involved), and the build job
    has to run them again because that is where the *packaged* copies live.
    """
    expected = {
        "gates": "Freedesktop metadata with the real validators",
        "build": "Validate the packaged metadata",
    }
    # The two steps name the files they validate by hand -- one runs against the
    # source tree, the other against the extracted package -- so a rename that
    # reached only one of them would leave the other validating a path that no
    # longer exists (and appstreamcli, given a missing file, is a hard error;
    # desktop-file-validate is too). Compare the names each step mentions to the
    # files that actually ship rather than to a literal that would rot.
    packaging = REPO / "snow_shot" / "packaging" / "linux"
    shipped = {p.name for p in packaging.iterdir()
               if p.name.endswith(".desktop") or p.name.endswith(".metainfo.xml")}
    check("the packaging directory ships a fixed set of metadata files",
          shipped == {"com.snowshot.snow_shot.desktop",
                      "com.snowshot.snow_shot.metainfo.xml"},
          f"found {sorted(shipped)}")
    for job_name, step_name in expected.items():
        job = jobs.get(job_name)
        if job is None:
            check(f"{job_name}: exists for the metadata validators", False, "no such job")
            continue
        steps = {step.get("name", ""): step for step in steps_of(job)}
        step = steps.get(step_name)
        check(f"{job_name}: has the step '{step_name}'", step is not None,
              "found: " + ", ".join(name for name in steps if name))
        if step is None:
            continue
        text = step.get("run", "")
        # Same flags in both places on purpose: `--no-net` keeps a runner
        # without network from validating a cached copy of the AppStream data,
        # and the two steps must not be able to disagree about it.
        for token in ("desktop-file-validate", "appstreamcli validate --no-net"):
            check(f"{job_name}: '{step_name}' runs {token}", token in text,
                  "the schema rules are only enforced by these two tools")
        named = {token.rsplit("/", 1)[-1] for token in re.findall(r"[^\s'\"=;]+", text)
                 if token.endswith(".desktop") or token.endswith(".metainfo.xml")}
        check(f"{job_name}: '{step_name}' validates the files that ship",
              named == shipped, f"names {sorted(named)}, ships {sorted(shipped)}")


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

            # A preset name never starts with `-` or a line-continuation `\`;
            # requiring that keeps the regex from reading the option that
            # follows a stripped `${{ }}` placeholder, or the backslash itself,
            # as the name. `[ \t]+` rather than `\s+` keeps it from walking into
            # the next line of the block -- which is how a stripped placeholder
            # ends up "naming" the following `cmake` invocation. The backslash
            # case is the AppImage step: it wraps its configure over several
            # lines and left `\` alone after the placeholder was stripped.
            for preset in re.findall(r"cmake --preset[ \t]+([^-\\\s]\S*)", literal):
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

        # The AppDir's entry point is executable only because `cmake -E copy`
        # carries the index's 100755 across. `test -x` on the source file is the
        # cheapest possible place to notice a checkout that lost the bit -- the
        # alternative is appimagetool failing half an hour into the build job.
        check("gates job checks the AppRun bit",
              re.search(r"test[ \t]+-x[ \t]+\S*packaging/linux/appimage/AppRun",
                        gates_text) is not None,
              "a lost mode bit would only surface in the build job")

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

        # The pinned dependency script is what supplies OpenCV 4.12 -- the
        # distribution's 4.6 has neither the `cv::barcode` API the barcode
        # service is written against nor the in-memory `WeChatQRCode`
        # constructor this repository adds with its own patch (§9.6.20). A
        # build job that stops building it would compile against the
        # distribution's OpenCV and fail three steps later, in the middle of
        # a translation unit that reads like a code bug.
        deps_script = REPO / "scripts" / "build-linux-deps.sh"
        deps_text = deps_script.read_text(encoding="utf-8") if deps_script.is_file() else ""
        deps_loop = re.search(r"^for library in (.+?); do", deps_text, re.MULTILINE)
        check("the pinned dependency script builds opencv",
              deps_loop is not None and "opencv" in deps_loop.group(1).split(),
              "the Linux build would use the distribution's OpenCV 4.6")
        check("the pinned dependency script applies the in-memory-model patch",
              "0024-contrib-wechat-qrcode-in-memory-models.patch" in deps_text,
              "the upstream WeChatQRCode has no in-memory constructor")
        check("the pinned dependency script builds opencv statically",
              "-DBUILD_SHARED_LIBS=OFF" in deps_text,
              "a shared libopencv_core.so.4.12 would be an unowned shared library "
              "in the .deb and would make dpkg-shlibdeps fail")

        # The Rust toolchain must be installed by hand and pinned with
        # RUSTUP_TOOLCHAIN.
        #
        # `rust-toolchain.toml` pins 1.97.1 *and* asks for the clippy and
        # rustfmt components. Inside the checkout, rustup honours that file
        # over the default toolchain, so the first `rustc`/`cargo` call makes
        # rustup install 1.97.1 with those components. On the arm64 runner the
        # pinned toolchain was half-installed, and rustup's recovery path rolled
        # the whole thing back after a component conflict (CI run
        # 39715098658): the rollback took rust-std with it, so the failure
        # surfaced much later as "Rust target 'aarch64-unknown-linux-gnu' is
        # not installed or usable" -- the target was never the problem.
        #
        # CI runs neither clippy nor rustfmt, so the fix is to install only
        # rustc/cargo/rust-std and pin the version with RUSTUP_TOOLCHAIN, which
        # outranks the toolchain file (verified with `rustup show`: "overridden
        # by environment variable RUSTUP_TOOLCHAIN") and so leaves rustup no
        # reason to install anything.
        for job_name in ("gates", "build"):
            job_text = "\n".join(run_commands(jobs.get(job_name, {})))
            check(f"{job_name} job installs the Rust toolchain by hand",
                  re.search(r"rustup toolchain install[ \t]+1\.97\.1", job_text) is not None,
                  "dtolnay/rust-toolchain leaves rust-toolchain.toml in charge of the components")
            check(f"{job_name} job pins RUSTUP_TOOLCHAIN",
                  "RUSTUP_TOOLCHAIN=1.97.1" in job_text,
                  "without it rustup honours rust-toolchain.toml and installs the components")
            check(f"{job_name} job does not install the clippy component",
                  "--component clippy" not in job_text and "--component clippy-preview" not in job_text,
                  "CI never runs clippy; installing it is what hit the conflict")
            check(f"{job_name} job does not use dtolnay/rust-toolchain",
                  "dtolnay/rust-toolchain" not in json.dumps(jobs.get(job_name, {})),
                  "that action leaves the toolchain file's components in charge")

        # The cross-target check adds two targets to the pinned toolchain; it
        # has to name that toolchain explicitly, because RUSTUP_TOOLCHAIN pins
        # which toolchain *cargo* uses but not which one `rustup target add`
        # writes to.
        check("gates job adds the cross targets to the pinned toolchain",
              re.search(r"rustup target add[ \t]+--toolchain[ \t]+1\.97\.1", gates_text) is not None,
              "the targets could land on a different toolchain than the checks use")

        # The apt lists, split by what each job is actually asked to do. The
        # requirements are not one list applied to every job: `gates` is defined
        # as "no toolchain" (it runs `cmake -P` script-mode checks, cargo and two
        # metadata validators), so demanding `qtbase5-private-dev` of it is a
        # demand to install a compiler header tree it never compiles against.
        # The distinction is read out of the job, not out of its name: a job
        # builds if it invokes the project build, and it validates if it runs a
        # validator. That keeps a renamed or added job honest in both directions.
        for job_name, job in jobs.items():
            text = "\n".join(run_commands(job))
            packages = apt_packages(text)
            if not packages:
                continue
            duplicates = {p for p in packages if packages.count(p) > 1}
            check(f"{job_name}: apt package list has no duplicates", not duplicates,
                  ", ".join(sorted(duplicates)))

            # The validators the port document defers to "the first command on
            # the real machine" have to be installed by whichever job runs them
            # or that step is a no-op that still exits zero on an empty grep.
            # Neither needs a compiler, so both jobs can carry this.
            if "desktop-file-validate" in text or "appstreamcli" in text:
                for tool in ("desktop-file-utils", "appstream"):
                    check(f"{job_name}: installs {tool}", tool in packages,
                          "missing from apt list")
            # `dpkg-dev` earns its place in two jobs for two reasons: `gates`
            # never builds a package (so it is not required there), and the
            # build job needs `dpkg-shlibdeps` for cpack's Depends and
            # `dpkg-deb` for the metadata step.
            if "cpack --config" in text or "dpkg-deb" in text:
                check(f"{job_name}: installs dpkg-dev", "dpkg-dev" in packages,
                      "missing from apt list")

            # Below this line the requirements are about compiling the project.
            # Two sources for the same library means a failed build silently
            # falls back to the old one, so `libopencv-dev` has to be absent
            # everywhere -- but the rest only makes sense where there is a
            # compiler.
            check(f"{job_name}: does not install libopencv-dev",
                  "libopencv-dev" not in packages,
                  "the distribution's OpenCV 4.6 would shadow the pinned 4.12")
            if "cmake --build --preset" not in text and "scripts/build-linux-deps.sh" not in text:
                continue
            # qtbase5-dev is public headers only: without qtbase5-private-dev
            # `Qt5::GuiPrivate` never exists, and the sources do include
            # `<qpa/...>` / `<private/...>` (CI run 39600792545).
            check(f"{job_name}: installs qtbase5-private-dev",
                  "qtbase5-private-dev" in packages,
                  "Qt5::GuiPrivate would be missing and 19 link lines would fail")
            # The OpenCV in-memory-model patch is applied with `patch`; the
            # script falls back to `git apply`, but `git` is already in the
            # list for the checkout, so `patch` is the explicit dependency.
            check(f"{job_name}: installs patch", "patch" in packages,
                  "the OpenCV in-memory-model patch could not be applied")
            # appimagetool shells out to mksquashfs. It is a separate package
            # from appimagetool, and on a runner it is the only thing standing
            # between a green build and "mksquashfs: command not found".
            if "Build the AppImage" in [step.get("name", "") for step in steps_of(job)]:
                check(f"{job_name}: installs squashfs-tools",
                      "squashfs-tools" in packages,
                      "appimagetool cannot build the squashfs without mksquashfs")

        appimage_checks(jobs)
        metadata_validator_checks(jobs)
        paths_ignore_checks(workflow)

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nLinux CI workflow check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
