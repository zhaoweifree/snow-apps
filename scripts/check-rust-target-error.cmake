# Fixture for the Rust-target probe error message in cmake/RustStaticLibrary.cmake.
#
# The probe runs `cargo metadata --filter-platform <target> --no-deps` and, when
# it fails, the message has to name the real remedy for the platform it is on:
# scripts/bootstrap.ps1 on Windows, a rustup command on Linux. It also has to
# carry cargo's own output -- in CI run 39715098658 the only useful line was
# the one ERROR_QUIET used to discard, and the message that replaced it blamed
# the target (which was fine) and pointed at a Windows script that does not
# exist on the Linux runners.
#
# The block under test is *extracted from the module and executed*, never
# copied here: a fixture that keeps its own copy of the message tests the
# copy, not the code.
#
# CMake cannot catch a message(FATAL_ERROR ...) in process, so the error text
# has to leave the process. This script therefore *is* the process that dies:
# the driver (scripts/check-rust-target-error.py) runs it once per platform
# and asserts on what it printed. Running it twice from here would mean a
# nested `cmake -P`, which the sandbox on this machine kills -- and a
# self-invoking fixture would be fragile on CI for the same reason.
#
# Usage:
#   cmake -P scripts/check-rust-target-error.cmake \
#         -DSNOW_FIXTURE_PLATFORM=linux|windows \
#         -DSNOW_FIXTURE_TARGET=<triple>
#
# The script is expected to exit non-zero, with the message on stderr.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SNOW_FIXTURE_PLATFORM)
    message(FATAL_ERROR "SNOW_FIXTURE_PLATFORM is required (linux or windows)")
endif()
if(NOT DEFINED SNOW_FIXTURE_TARGET)
    message(FATAL_ERROR "SNOW_FIXTURE_TARGET is required")
endif()

set(_module_path "${CMAKE_CURRENT_LIST_DIR}/../cmake/RustStaticLibrary.cmake")

file(READ "${_module_path}" _module)
# The module uses LF line endings on Linux but CRLF in a Windows checkout.
# Splitting on "\n" alone would leave a stray "\r" at the end of every line,
# and that carriage return ends up inside the extracted block -- where it
# splits a CMake string across a line break and corrupts the message.
string(REPLACE "\r\n" "\n" _module "${_module}")
string(REPLACE "\n" ";" _module_lines "${_module}")

# ---------------------------------------------------------------------------
# Locate the block under test.
#
# The block is the whole probe: the execute_process() that runs cargo, plus
# the if() that turns its failure into the message.
#
# The search is done on the raw text, not on a list of lines. Splitting the
# text on newlines would break the multi-line string literals inside the
# block, and the block under test has one:
#     string(CONCAT _rust_remedy
#         "Install it with: rustup toolchain install 1.97.1 --profile minimal "
#         "--target ${SNOW_RUST_TARGET}")
# Cut into lines, the two literals would become two statements, and the
# message would end up saying "1.97.1 --profile minimal ;--target ...".
#
# (That was not a fixture artefact: set() with two values builds a *list*,
# so the ';' was in the real message too, until the block was changed to
# string(CONCAT). The gate in check-rust-target-error.py now asserts the
# separator is gone.)
#
# So: find the markers by string search, then count the if()/endif() nesting
# over the lines of that range only.
# ---------------------------------------------------------------------------

# 1. the `if(NOT _metadata_result EQUAL 0)` that guards the message
string(FIND "${_module}" "if(NOT _metadata_result EQUAL 0)" _if_pos)
if(_if_pos LESS 0)
    message(FATAL_ERROR
        "could not find 'if(NOT _metadata_result EQUAL 0)' in ${_module_path}")
endif()

# 2. the execute_process() that feeds it -- the last one before the if()
string(SUBSTRING "${_module}" 0 ${_if_pos} _before_if)
string(REGEX MATCHALL "[^\n]*execute_process[(][^\n]*" _exec_hits "${_before_if}")
list(LENGTH _exec_hits _exec_count)
if(_exec_count EQUAL 0)
    message(FATAL_ERROR
        "could not find the execute_process() before the guard in ${_module_path}")
endif()
# The nearest one above the guard is the right one.
math(EXPR _exec_last "${_exec_count} - 1")
list(GET _exec_hits ${_exec_last} _exec_line)

# The offset of that execute_process() inside the whole text.
string(FIND "${_module}" "${_exec_line}" _exec_pos)

# 3. the matching endif(): count the nesting from the if() forward.
#    The nesting is counted over the *matches* of if()/endif() in the tail,
#    not over lines: a line-oriented split loses the offsets, and the block
#    itself is cut out of the original text below, so a multi-line string
#    literal inside it stays intact.
set(_tail_start ${_if_pos})
string(SUBSTRING "${_module}" ${_tail_start} -1 _tail)
set(_depth 1)
set(_end_pos -1)
# The tail starts at the guard itself, so the guard is already counted in the
# initial depth of 1. Every `if(`/`endif(` after it is matched one at a time
# and the rest of the tail is consumed, so the offsets stay exact.
set(_rest "${_tail}")
set(_abs 0)
set(_skip_first TRUE)
while(TRUE)
    string(REGEX MATCH "[ \t]*(if|endif)[(]" _match "${_rest}")
    if(NOT _match)
        break()
    endif()
    string(FIND "${_rest}" "${_match}" _match_pos)
    if(_skip_first)
        set(_skip_first FALSE)
    elseif(_match MATCHES "endif[(]")
        math(EXPR _depth "${_depth} - 1")
        if(_depth EQUAL 0)
            # The match is the indentation plus "endif(" -- the closing "f)"
            # and ")" come after it, so the block ends two characters later.
            string(LENGTH "${_match}" _match_len)
            math(EXPR _end_pos "${_tail_start} + ${_abs} + ${_match_pos} + ${_match_len} + 2")
            break()
        endif()
    else()
        math(EXPR _depth "${_depth} + 1")
    endif()
    # consume the matched token
    string(LENGTH "${_match}" _match_len)
    math(EXPR _consume "${_match_pos} + ${_match_len}")
    string(SUBSTRING "${_rest}" ${_consume} -1 _rest)
    math(EXPR _abs "${_abs} + ${_consume}")
endwhile()

if(_end_pos LESS 0)
    message(FATAL_ERROR
        "could not find the matching endif() in ${_module_path}")
endif()

# 4. cut the block out
math(EXPR _block_len "${_end_pos} - ${_exec_pos} + 1")
string(SUBSTRING "${_module}" ${_exec_pos} ${_block_len} _snippet)

# ---------------------------------------------------------------------------
# A fake cargo that fails the way the real one did, with its message on
# stderr -- the message the old code discarded.
#
# The scratch directory goes to the system temp, not to the current directory:
# in script mode CMAKE_CURRENT_BINARY_DIR is the cwd, so the fixture would
# drop a fake-cargo/ directory into the repository every time it ran.
# ---------------------------------------------------------------------------
if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(_tmp_root "$ENV{TMPDIR}")
elseif(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
    set(_tmp_root "$ENV{TEMP}")
else()
    set(_tmp_root "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(_fake_dir "${_tmp_root}/snow-rust-target-error-fixture")
file(REMOVE_RECURSE "${_fake_dir}")
file(MAKE_DIRECTORY "${_fake_dir}")
# The fake cargo has to be a native script of the host. On the Linux CI
# runners that is a .sh; on this Windows workstation it is a .bat. The
# override lets the other branch be exercised on either host -- otherwise the
# Unix branch would only ever run on CI, and a break there would land as a
# red gate instead of as a local failure.
if(DEFINED SNOW_FIXTURE_FAKE_SH)
    set(_use_sh TRUE)
else()
    if(CMAKE_HOST_WIN32)
        set(_use_sh FALSE)
    else()
        set(_use_sh TRUE)
    endif()
endif()
if(_use_sh)
    set(_fake_script "${_fake_dir}/cargo.sh")
    file(WRITE "${_fake_script}"
        "#!/bin/sh\necho 'error: failed to run custom build command for snow_rust_ffi' >&2\nexit 1\n")
    file(CHMOD "${_fake_script}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)
    if(CMAKE_HOST_WIN32)
        # A .sh is not executable on Windows, so the script is driven through
        # the shell instead. The script itself stays the Unix one -- what is
        # under test is that the block captures a failing cargo's stderr, and
        # that has to hold for the .sh shape the CI runners use.
        find_program(_sh_executable NAMES sh sh.exe)
        if(NOT _sh_executable)
            message(FATAL_ERROR
                "SNOW_FIXTURE_FAKE_SH was requested but no 'sh' was found on PATH; "
                "the Unix fake-cargo branch cannot be exercised on this host")
        endif()
        set(_fake "${_fake_dir}/cargo.bat")
        file(WRITE "${_fake}"
            "@echo off\r\n\"${_sh_executable}\" \"${_fake_script}\" %*\r\n")
    else()
        set(_fake "${_fake_script}")
    endif()
else()
    set(_fake "${_fake_dir}/cargo.bat")
    file(WRITE "${_fake}"
        "@echo error: failed to run custom build command for snow_rust_ffi 1>&2\r\nexit /b 1\r\n")
endif()

set(CARGO_EXECUTABLE "${_fake}")
set(SNOW_RUST_TARGET "${SNOW_FIXTURE_TARGET}")
# The block runs cargo with WORKING_DIRECTORY set to the manifest dir.
set(SNOW_RUST_MANIFEST_DIR "${_fake_dir}")

# `WIN32` is a normal variable in script mode, so the fixture can select the
# platform branch it wants to exercise.
if(SNOW_FIXTURE_PLATFORM STREQUAL "windows")
    set(WIN32 TRUE)
else()
    unset(WIN32)
endif()

# The extracted block is written to its own file and included. Including this
# script would re-enter it, and the block would never run.
set(_snippet_file "${_fake_dir}/error-block.cmake")
file(WRITE "${_snippet_file}" "${_snippet}")
include("${_snippet_file}")

# The block ends in message(FATAL_ERROR ...), which aborts the script.
message(FATAL_ERROR "the error block did not abort; the fixture is not testing anything")
