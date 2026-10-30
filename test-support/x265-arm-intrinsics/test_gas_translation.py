#!/usr/bin/env python3
"""Exercise x265's opt-in ARMASM64 build commands without claiming assembly validation.

On a POSIX host, Clang preprocesses with Windows/MSVC macros and the repository's
pinned GAS-preprocessor translates all baseline sources. A recording assembler
copies its input instead of producing machine code. Native Windows validation
must use the real ARMASM64 executable and the CMake encoder/primitive fixture.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SOURCES = (
    "asm.S mc-a.S mc-a-common.S sad-a.S pixel-util.S pixel-util-common.S "
    "p2s.S p2s-common.S blockcopy8.S blockcopy8-common.S ssd-a.S "
    "ssd-a-common.S intrapred.S dct.S"
).split()


def bracket(value: str | Path) -> str:
    return "[==[" + str(value) + "]==]"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def executable(path: Path, source: str) -> None:
    path.write_text("#!" + sys.executable + "\n" + source, encoding="utf-8")
    path.chmod(0o755)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Patched x265 4.1 source root")
    parser.add_argument("--gas-preprocessor", type=Path, required=True)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--perl", default=shutil.which("perl"))
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--build-root", type=Path, default=Path("build"))
    args = parser.parse_args()
    require(os.name != "nt", "Use the real MSVC fixture on Windows; this probe uses POSIX tool shims")
    require(all((args.clang, args.perl, args.cmake)), "Clang, Perl, and CMake must already be available")
    source = args.source.resolve()
    gaspp = args.gas_preprocessor.resolve()
    module = source / "source/cmake/WindowsArm64Assembly.cmake"
    require(module.is_file() and gaspp.is_file(), "Patched assembly module and GAS-preprocessor required")
    args.build_root.mkdir(parents=True, exist_ok=True)
    # Exercise the real top-level/common configuration before testing isolated
    # assembly commands, including ARMASM64's empty per-source compiler flag.
    subprocess.run([
        sys.executable, str(Path(__file__).with_name("test_configuration.py")),
        "--source", str(source), "--cmake", args.cmake,
        "--build-root", str(args.build_root),
    ], check=True)
    with tempfile.TemporaryDirectory(prefix="x265 gas translation ", dir=args.build_root.resolve()) as temp:
        work = Path(temp)
        tools = work / "host tools"
        tools.mkdir()
        cl_log = work / "preprocessor-arguments.jsonl"
        executable(
            tools / "cl",
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "with Path(os.environ['SNOW_GASPP_CL_LOG']).open('a') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "arguments = [a for a in sys.argv[1:] if a != '-nologo']\n"
            "os.execv(os.environ['SNOW_GASPP_CLANG'], ['clang', '-x', 'assembler-with-cpp', "
            "'-undef', '-D_WIN32', '-D_M_ARM64', '-D_MSC_VER=1951'] + arguments)\n",
        )
        assembler = tools / "record-armasm64"
        executable(
            assembler,
            "import json, shutil, sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "output = Path(args[args.index('-o') + 1])\n"
            "assembly = next(Path(a) for a in args if a.endswith('.asm'))\n"
            "shutil.copyfile(assembly, str(output) + '.translated.asm')\n"
            "Path(str(output) + '.arguments.json').write_text(json.dumps(args))\n"
            "output.write_bytes(b'translation fixture only; no machine code')\n",
        )
        project = work / "project"
        project.mkdir()
        assembly_dir = source / "source/common/aarch64"
        cmake = [
            "cmake_minimum_required(VERSION 3.22)",
            "project(X265GasTranslation LANGUAGES NONE)",
            "set(PERL_EXECUTABLE " + bracket(Path(args.perl).resolve()) + ")",
            "set(X265_GASPREPROCESSOR " + bracket(gaspp) + ")",
            "set(X265_ARMASM64_EXECUTABLE " + bracket(assembler) + ")",
            "include(" + bracket(module) + ")",
        ]
        for depth, namespace in ((8, "x265"), (10, "x265_10bit")):
            subproject = project / str(depth)
            subproject.mkdir()
            definitions = f"-DHIGH_BIT_DEPTH={int(depth > 8)} -DBIT_DEPTH={depth} -DX265_NS={namespace}"
            commands = ["set(ASM_FLAGS " + definitions + ")"]
            for name in SOURCES:
                commands += [
                    "x265_add_msvc_arm64_assembly(object " + bracket(assembly_dir / name) + ")",
                    'list(APPEND objects "${object}")',
                ]
            commands += [f"add_custom_target(translate-{depth} ALL DEPENDS ${{objects}})"]
            (subproject / "CMakeLists.txt").write_text("\n".join(commands) + "\n", encoding="utf-8")
            cmake += [f"add_subdirectory({depth})"]
        (project / "CMakeLists.txt").write_text("\n".join(cmake) + "\n", encoding="utf-8")
        build = work / "generated commands"
        environment = dict(
            os.environ,
            PATH=str(tools) + os.pathsep + os.environ["PATH"],
            SNOW_GASPP_CL_LOG=str(cl_log),
            SNOW_GASPP_CLANG=str(Path(args.clang).resolve()),
        )
        validation = work / "validation project"
        validation.mkdir()
        validation_cmake = [
            "cmake_minimum_required(VERSION 3.22)",
            "project(X265ArmAsmValidation LANGUAGES NONE)",
        ]
        for variable in ("MSVC", "ARM64", "WIN32", "ENABLE_ASSEMBLY"):
            validation_cmake += [f"option({variable} \"Fixture platform setting\" ON)"]
        for extension in ("ENABLE_NEON_DOTPROD", "ENABLE_NEON_I8MM", "ENABLE_SVE", "ENABLE_SVE2"):
            validation_cmake += [f"option({extension} \"Fixture ISA setting\" OFF)"]
        validation_cmake += ["include(" + bracket(module) + ")", "x265_validate_msvc_arm64_assembly()"]
        (validation / "CMakeLists.txt").write_text("\n".join(validation_cmake) + "\n", encoding="utf-8")
        cases = [None] + [
            f"{variable}=OFF" for variable in ("MSVC", "ARM64", "WIN32", "ENABLE_ASSEMBLY")
        ] + [
            f"{extension}=ON" for extension in
            ("ENABLE_NEON_DOTPROD", "ENABLE_NEON_I8MM", "ENABLE_SVE", "ENABLE_SVE2")
        ]
        for index, setting in enumerate(cases):
            command = [args.cmake, "-S", str(validation), "-B", str(work / f"validation-{index}")]
            if setting:
                command += ["-D" + setting]
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            require((result.returncode == 0) == (setting is None),
                    f"MSVC ARM64 baseline validation did not reject/accept {setting}: {result.stderr}")
        subprocess.run([args.cmake, "-S", str(project), "-B", str(build)], env=environment, check=True)
        subprocess.run([args.cmake, "--build", str(build), "--parallel", "1"], env=environment, check=True)
        preprocessor_calls = [json.loads(line) for line in cl_log.read_text().splitlines()]
        require(len(preprocessor_calls) == 28, "All 14 source files must preprocess in both bit depths")
        exports = 0
        for depth, namespace in ((8, "x265"), (10, "x265_10bit")):
            calls = [call for call in preprocessor_calls if f"-DBIT_DEPTH={depth}" in call]
            require(len(calls) == 14, "Each depth must have 14 distinct build commands")
            require(
                all(f"-DHIGH_BIT_DEPTH={int(depth > 8)}" in call and
                    f"-DX265_NS={namespace}" in call and "-DPIC=1" in call for call in calls),
                "Depth, storage, Main10 namespace, and Windows relocations must reach preprocessing",
            )
            require(all(not any(arg.startswith("-march") for arg in call) for call in calls),
                    "MSVC build commands must not receive GAS compiler ISA flags")
            for name in SOURCES:
                object_file = build / str(depth) / (name + ".obj")
                translated = Path(str(object_file) + ".translated.asm").read_text()
                assembler_arguments = json.loads(Path(str(object_file) + ".arguments.json").read_text())
                require("-nologo" in assembler_arguments, "ARMASM64 must receive native options")
                require(not any(arg.startswith(("-D", "-I", "-march")) for arg in assembler_arguments),
                        "GAS-preprocessor must strip preprocessing flags before invoking ARMASM64")
                for line in translated.splitlines():
                    if not line.strip() or line.lstrip().startswith(";"):
                        continue
                    require(not re.search(r"^\s*[.@]|\\|%function", line),
                            f"Untranslated GAS syntax in {depth}/{name}: {line}")
                    if line.lstrip().startswith("EXPORT "):
                        require(line.strip().split()[1].startswith(namespace + "_"),
                                "Exported assembly symbols must preserve their bit-depth namespace")
                        exports += 1
                require(translated.rstrip().endswith("END"), "ARMASM64 unit must terminate with END")
        require(exports > 500, "The fixture must exercise real expanded primitive bodies")
        print(f"28/28 baseline translation commands passed; {exports} namespaced exports checked")
        print("Baseline configuration accepted; all 8 invalid platform/ISA configurations rejected")
        print("Translation only: ARMASM64 assembly, linkage, ABI, and native execution remain unverified")


if __name__ == "__main__":
    main()
