#!/usr/bin/env python3
"""Configure the real patched x265 project with empty and nonempty NEON flags.

The ARMASM64 cases simulate Windows/MSVC CMake branches on a POSIX host. Native
compilers perform configuration checks only; no encoder or assembly is built.
This catches integration errors that the isolated GAS translation fixture misses.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


PROJECT = r'''
cmake_minimum_required(VERSION 3.22)
project(X265ConfigurationRegression LANGUAGES C CXX)

# Keep the host compilers for configure-time probes while selecting x265's
# AArch64 rules. We never compile the generated encoder or assembly targets.
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_OSX_ARCHITECTURES "")
if(SNOW_TEST_ARMASM64)
    set(MSVC TRUE)
    set(WIN32 TRUE)
    set(expected_flags "")
else()
    set(expected_flags "-march=armv8-a")
endif()
set(ENABLE_MSVC_ARM64_ASSEMBLY "${SNOW_TEST_ARMASM64}" CACHE BOOL "" FORCE)
set(ENABLE_ASSEMBLY ON CACHE BOOL "" FORCE)
set(ENABLE_ARM_INTRINSICS ON CACHE BOOL "" FORCE)
foreach(extension IN ITEMS NEON_DOTPROD NEON_I8MM SVE SVE2)
    set(ENABLE_${extension} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBNUMA OFF CACHE BOOL "" FORCE)
set(MAIN12 OFF CACHE BOOL "" FORCE)
set(VERSION 4.1)
# The assembly helper checks tool existence when generating commands. These
# placeholders are never executed: native ARMASM64 validation is a separate test.
set(PERL_EXECUTABLE "${CMAKE_COMMAND}")
set(X265_GASPREPROCESSOR "${CMAKE_COMMAND}")
set(X265_ARMASM64_EXECUTABLE "${CMAKE_COMMAND}")
add_subdirectory("${SNOW_X265_SOURCE}/source" x265)

get_target_property(common_directory common SOURCE_DIR)
get_target_property(common_sources common SOURCES)
foreach(kernel IN ITEMS asm-primitives pixel-prim filter-prim dct-prim
                        loopfilter-prim intrapred-prim arm64-utils sao-prim)
    set(source "aarch64/${kernel}.cpp")
    if(NOT source IN_LIST common_sources)
        message(FATAL_ERROR "Missing baseline NEON source: ${source}")
    endif()
    get_source_file_property(flags "${common_directory}/${source}"
        DIRECTORY "${common_directory}" COMPILE_FLAGS)
    if(NOT "${flags}" STREQUAL "${expected_flags}")
        message(FATAL_ERROR "${source}: expected '${expected_flags}', got '${flags}'")
    endif()
endforeach()
get_directory_property(definitions DIRECTORY "${SNOW_X265_SOURCE}/source" COMPILE_DEFINITIONS)
if(HIGH_BIT_DEPTH)
    set(expected_depth X265_DEPTH=10)
    set(expected_namespace X265_NS=x265_10bit)
else()
    set(expected_depth X265_DEPTH=8)
    set(expected_namespace X265_NS=x265)
endif()
if(NOT expected_depth IN_LIST definitions OR NOT expected_namespace IN_LIST definitions)
    message(FATAL_ERROR "The configured encoder must retain its depth and namespace")
endif()
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Patched x265 4.1 source root")
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--build-root", type=Path, default=Path("build"))
    args = parser.parse_args()
    if os.name == "nt":
        parser.error("Use the native MSVC fixture on Windows; this probe simulates it on POSIX")
    if not args.cmake:
        parser.error("CMake must already be available")
    source = args.source.resolve()
    if not (source / "source/cmake/WindowsArm64Assembly.cmake").is_file():
        parser.error("--source must contain the complete patched x265 source tree")
    args.build_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="x265 configuration ", dir=args.build_root.resolve()) as temp:
        work = Path(temp)
        (work / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        for armasm64 in (True, False):
            for depth in (8, 10):
                name = f"{'armasm64' if armasm64 else 'native'}-main{depth}"
                command = [
                    args.cmake, "-S", str(work), "-B", str(work / name),
                    f"-DSNOW_X265_SOURCE={source}",
                    f"-DSNOW_TEST_ARMASM64={'ON' if armasm64 else 'OFF'}",
                    f"-DHIGH_BIT_DEPTH={'ON' if depth == 10 else 'OFF'}",
                    f"-DEXPORT_C_API={'OFF' if depth == 10 else 'ON'}",
                ]
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(f"{name} configuration failed:\n{result.stdout}{result.stderr}")
                print(f"{name}: configuration and NEON source flags passed")
    print("Configuration only: native Windows compilation and execution remain unverified")


if __name__ == "__main__":
    main()
