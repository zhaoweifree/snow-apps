# Local gate for the Linux packaging configuration.
#
# Run with `cmake -P scripts/check-linux-packaging.cmake` (the shell entry point
# `scripts/check-linux-packaging.sh` also runs the freedesktop checks). It works
# on any host because the packaging module is only ever *included* on Linux:
# without this script, a typo in `cmake/SnowShotLinuxPackage.cmake` would stay
# invisible until somebody configured a Linux tree, which is exactly the blind
# spot the port document keeps warning about.
#
# `CPACK_DEBIAN_PACKAGE_ARCHITECTURE` gets two tests: one per supported
# processor name, and one that an unknown processor fails instead of producing a
# package dpkg rejects.
cmake_minimum_required(VERSION 3.21)

get_filename_component(_snow_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The inputs the top-level CMakeLists.txt normally provides.
set(SNOW_SHOT_VERSION "1.2.8")
set(SNOW_SHOT_VERSION_NUMERIC "1.2.8")
set(SNOW_SHOT_PRODUCT_NAME "Snow Shot")
set(SNOW_SHOT_EXECUTABLE_NAME "snow_shot")
set(SNOW_SHOT_VENDOR "Snow Apps")
set(SNOW_SHOT_DESCRIPTION
    "A screenshot utility for capturing, annotating, pinning, and recognizing screen content.")
set(SNOW_SHOT_HOMEPAGE_URL "https://snowshot.top")
set(SNOW_SHOT_SUPPORT_EMAIL "chao@mgchao.top")
set(CMAKE_PROJECT_NAME "snow-apps")
# A writable stand-in for the build directory. Every path this script checks is
# built from this variable, so the value only has to look like a build tree --
# but it does have to *exist*: `snow_shot_configure_linux_package()` configures
# the CPack bundle script into `${CMAKE_BINARY_DIR}`, and `configure_file` cannot
# write into a directory that is not there. A literal `/nonexistent/snow-build`
# looked harmless on the Windows workstation (the path resolved somewhere
# writable) and failed the gate on Linux:
#
#     CMake Error: Could not open file for write in copy operation
#     /nonexistent/snow-build/snow-shot-linux-runtime-bundle.cmake.tmp
#
# Cleared at the start so a run that failed halfway does not leave state behind
# for the next one.
set(CMAKE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/snow-packaging-build")
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_SYSTEM_PROCESSOR "x86_64")

include("${_snow_repo}/cmake/SnowShotLinuxPackage.cmake")

set(_snow_failures 0)

function(snow_expect label expected actual)
    if(NOT "${expected}" STREQUAL "${actual}")
        message(STATUS "FAIL ${label}: expected '${expected}', got '${actual}'")
        math(EXPR _failures "${_snow_failures} + 1")
        set(_snow_failures "${_failures}" PARENT_SCOPE)
    else()
        message(STATUS "ok   ${label} = ${actual}")
    endif()
endfunction()

# --- 1. Processor name -> Debian architecture ---------------------------------
snow_shot_linux_debian_architecture(_arch)
snow_expect("host architecture" "amd64" "${_arch}")

foreach(_case
        "x86_64:amd64"
        "AMD64:amd64"
        "aarch64:arm64"
        "arm64:arm64"
        "loongarch64:loong64"
        "mips64el:mips64el"
        "riscv64:riscv64")
    string(REPLACE ":" ";" _pair "${_case}")
    list(GET _pair 0 _processor)
    list(GET _pair 1 _expected)
    snow_shot_linux_debian_architecture(_mapped PROCESSOR "${_processor}")
    snow_expect("${_processor}" "${_expected}" "${_mapped}")
endforeach()

# An unsupported processor must abort rather than emit a package no dpkg will
# accept. The check runs in a child interpreter because FATAL_ERROR ends the
# current script.
set(_snow_negative_script "${CMAKE_CURRENT_BINARY_DIR}/snow-packaging-negative.cmake")
if(NOT CMAKE_CURRENT_BINARY_DIR)
    set(_snow_negative_script "${CMAKE_BINARY_DIR}/cmake/check-linux-packaging-negative.cmake")
endif()
file(WRITE "${_snow_negative_script}"
    "include(\"${_snow_repo}/cmake/SnowShotLinuxPackage.cmake\")\n\
snow_shot_linux_debian_architecture(_arch PROCESSOR \"sparc64\")\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${_snow_negative_script}"
    RESULT_VARIABLE _snow_negative_result
    OUTPUT_QUIET
    ERROR_QUIET)
file(REMOVE "${_snow_negative_script}")
if(_snow_negative_result EQUAL 0)
    message(STATUS "FAIL unsupported processor: the mapping accepted sparc64")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   unsupported processor rejected (exit ${_snow_negative_result})")
endif()

# --- 2. The CPack variables the DEB generator reads --------------------------
snow_shot_configure_linux_package()
snow_expect("CPACK_GENERATOR" "DEB" "${CPACK_GENERATOR}")
snow_expect("CPACK_PACKAGE_NAME" "snow-shot" "${CPACK_PACKAGE_NAME}")
snow_expect("CPACK_PACKAGE_VERSION" "1.2.8" "${CPACK_PACKAGE_VERSION}")
snow_expect("CPACK_PACKAGING_INSTALL_PREFIX" "/usr" "${CPACK_PACKAGING_INSTALL_PREFIX}")
snow_expect("CPACK_DEBIAN_FILE_NAME" "DEB-DEFAULT" "${CPACK_DEBIAN_FILE_NAME}")
snow_expect("CPACK_DEBIAN_PACKAGE_ARCHITECTURE" "amd64" "${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
snow_expect("CPACK_DEBIAN_PACKAGE_SECTION" "graphics" "${CPACK_DEBIAN_PACKAGE_SECTION}")
snow_expect("CPACK_DEBIAN_PACKAGE_SHLIBDEPS" "ON" "${CPACK_DEBIAN_PACKAGE_SHLIBDEPS}")
snow_expect("CPACK_PACKAGE_EXECUTABLES" "snow_shot;Snow Shot" "${CPACK_PACKAGE_EXECUTABLES}")
snow_expect("CPACK_COMPONENTS_ALL" "SnowShot" "${CPACK_COMPONENTS_ALL}")
if(NOT CPACK_DEBIAN_PACKAGE_MAINTAINER MATCHES "^Snow Apps <.+@.+>$")
    message(STATUS "FAIL CPACK_DEBIAN_PACKAGE_MAINTAINER: '${CPACK_DEBIAN_PACKAGE_MAINTAINER}'"
                   " is not 'Vendor <mail>'")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()
# A hard-coded Depends list would be a guess; only the shlibdeps path is allowed.
if(CPACK_DEBIAN_PACKAGE_DEPENDS)
    message(STATUS "FAIL CPACK_DEBIAN_PACKAGE_DEPENDS is set: ${CPACK_DEBIAN_PACKAGE_DEPENDS}")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# --- 2b. The private libraries of the project itself ------------------------
# `dpkg-shlibdeps` fails outright on a NEEDED soname that no installed package
# provides, and every pinned dependency of this project is in that situation
# (`libjxl.so.0.11`, `libOpenEXR-3_4.so.33`, `liblunasvg.so.3` ...). Three
# separate pieces have to exist for the package to build *and* run; a missing
# one is invisible until CPack runs on a real Linux host.
#
# The hook is `install(SCRIPT ...)`, not `CPACK_INSTALL_SCRIPTS`. The latter is
# executed *before* the project is installed (run 38: the script ran, found an
# empty staging tree and reported "the staged tree has no ELF objects"), and it
# does not run at all for `cmake --install`, which is how the AppImage stages its
# AppDir. An install rule runs after the install rules that precede it and runs
# for both consumers.
set(_module_text_source "${_snow_repo}/cmake/SnowShotLinuxPackage.cmake")
file(READ "${_module_text_source}" _module_text)
string(REGEX REPLACE "#[^\n]*" "" _module_code "${_module_text}")
if(NOT _module_code MATCHES "install\\(SCRIPT")
    message(STATUS "FAIL the packaging module never registers the bundle script as an "
                   "install rule; CPACK_INSTALL_SCRIPTS runs before the tree exists")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the bundle script runs as an install rule")
endif()
# A rule with no COMPONENT belongs to the top-level directory's default
# component, which is `Unspecified` -- `CMAKE_INSTALL_DEFAULT_COMPONENT_NAME` is
# set inside `snow_shot/CMakeLists.txt` and does not reach the top level. CPack
# installs only `SnowShot`, so the rule would be skipped for the .deb and still
# run for `cmake --install`: the two consumers would disagree.
if(_module_code MATCHES "install\\(SCRIPT[^)]*COMPONENT")
    message(STATUS "ok   the install rule names its component")
else()
    message(STATUS "FAIL the bundle install rule has no COMPONENT; CPack installs only "
                   "SnowShot and would skip it")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

if(_module_code MATCHES "CPACK_INSTALL_SCRIPTS")
    message(STATUS "FAIL the packaging module still uses CPACK_INSTALL_SCRIPTS; it runs "
                   "before the staged tree is filled")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the packaging module does not use CPACK_INSTALL_SCRIPTS")
endif()
if(NOT SNOW_SHOT_LINUX_BUNDLE_SCRIPT)
    message(STATUS "FAIL the packaging function exported no bundle script path: nothing "
                   "would bundle the self-built libraries, and dpkg-shlibdeps fails on "
                   "the first one it cannot find")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the packaging function exports the bundle script")
    foreach(_script IN ITEMS "${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}")
        if(NOT EXISTS "${_script}")
            message(STATUS "FAIL the generated bundle script does not exist: ${_script}")
            math(EXPR _snow_failures "${_snow_failures} + 1")
            continue()
        endif()
        file(READ "${_script}" _bundle_text)
        if(_bundle_text MATCHES "@[A-Z_]+@")
            message(STATUS "FAIL the bundle script still has an unsubstituted placeholder")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        endif()
        # Comments are stripped first. The template explains what these calls
        # do, and a plain substring match on the name stays green when the call
        # is deleted -- the same trap as the `continuous` tag check in
        # scripts/check-linux-appimage.py, which matched its own comment.
        string(REGEX REPLACE "#[^\n]*" "" _bundle_code "${_bundle_text}")
        # `RPATH_SET FILE` / `NEW_RPATH` as two needles, not the bare name: the
        # positional form `file(RPATH_SET <file> <rpath>)` names the sub-command
        # too and still aborts CPack with "unrecognized argument", which is
        # exactly what the first version of this script did. The functional
        # probe below is what makes the keyword form non-negotiable.
        foreach(_needle
                "GET_RUNTIME_DEPENDENCIES"
                "CMAKE_INSTALL_PREFIX"
                "RPATH_SET FILE"
                "NEW_RPATH"
                "-DRPATH_FILE="
                "No valid ELF RPATH or RUNPATH entry")
            if(NOT _bundle_code MATCHES "${_needle}")
                message(STATUS "FAIL the bundle script never uses ${_needle}")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            else()
                message(STATUS "ok   the bundle script uses ${_needle}")
            endif()
        endforeach()
        # The solver searches `DIRECTORIES` plus the *host* system paths. The
        # project installs one shared library of its own into `usr/lib` rather
        # than into the private directory, so on a host where that path is not a
        # system directory it looks unresolved:
        #
        #     snow-shot bundle: unresolved libsnow_shot_image_codec_backend.so
        #
        # A dependency that is in the tree and still reported missing hides the
        # next one that really is. `string(FIND)` and not a regex: the needle is
        # full of `${}` and `"`.
        set(_directories_needle
            [=[DIRECTORIES "${_deps_lib_dir}" "${_staged}/${_lib_subdir}"]=])
        string(FIND "${_bundle_code}" "${_directories_needle}" _directories_position)
        if(_directories_position EQUAL -1)
            message(STATUS "FAIL the bundle script searches only the dependency prefix; "
                           "the project's own libraries in the staged libdir then read "
                           "as unresolved")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        else()
            message(STATUS "ok   the bundle script searches the staged library directory too")
        endif()
        # The copy has to be named after the soname the loader asks for, which is
        # the name `GET_RUNTIME_DEPENDENCIES` resolved -- not the name of the file
        # the symlink points at. Taking the name from `REALPATH` ships
        # `libjxl.so.0.11.0` in a package whose every NEEDED entry says
        # `libjxl.so.0.11`, and the binary then fails at startup on the target
        # machine, long after this gate could have said so.
        set(_soname_needle [=[get_filename_component(_soname "${_dependency}" NAME)]=])
        string(FIND "${_bundle_code}" "${_soname_needle}" _soname_position)
        if(_soname_position EQUAL -1)
            message(STATUS "FAIL the bundled copy is not named after the resolved soname")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        else()
            message(STATUS "ok   the bundled copy is named after the resolved soname")
        endif()
        # The staged root has to be the one the caller named. A bare `STAGED_DIR`
        # substring match would stay green on a script that mentions the override
        # in a message and then ignores it.
        set(_staged_needle [=[set(_staged "${STAGED_DIR}")]=])
        string(FIND "${_bundle_code}" "${_staged_needle}" _staged_position)
        if(_staged_position EQUAL -1)
            message(STATUS "FAIL the bundle script ignores the STAGED_DIR override")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        else()
            message(STATUS "ok   the bundle script honours the STAGED_DIR override")
        endif()
        # `CMAKE_INSTALL_PREFIX` is the prefix *inside* the package, not a
        # directory on the build host: with `CPACK_SET_DESTDIR` off, CPack passes
        # the packaging prefix (`/usr`) and reaches the staging tree through the
        # DESTDIR mechanism, which is also what every generated install rule
        # applies to its destination. Taking the prefix alone pointed the glob at
        # the host's `/usr/bin` (run 39: 3000 entries, and the loop handed
        # `file(READ)` a 43 KB "file name"). Two needles, because either one
        # alone leaves a script that reads the host.
        foreach(_needle
                [=[set(_staged "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")]=]
                "there is no staging tree")
            string(FIND "${_bundle_code}" "${_needle}" _staging_position)
            if(_staging_position EQUAL -1)
                message(STATUS "FAIL the bundle script does not build its staging tree from "
                               "DESTDIR (missing: ${_needle})")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            else()
                message(STATUS "ok   the bundle script builds its staging tree from DESTDIR")
            endif()
        endforeach()
        # Every staged glob goes through the helper, which is what turns a
        # collapsed `file(GLOB)` result (see _snow_staged_paths) into a sentence
        # instead of an ENAMETOOLONG.
        foreach(_needle
                "_snow_staged_paths(_staged_programs"
                "_snow_staged_paths(_staged_libraries")
            string(FIND "${_bundle_code}" "${_needle}" _glob_position)
            if(_glob_position EQUAL -1)
                message(STATUS "FAIL the bundle script does not validate the glob behind "
                               "${_needle}")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            else()
                message(STATUS "ok   the bundle script validates the glob behind ${_needle}")
            endif()
        endforeach()
        # The bindir has to be the real one. This gate runs in script mode, where
        # nothing ever ran `GNUInstallDirs`, so the module substitutes whatever it
        # finds -- and an empty value turns the glob into `${_staged}//*`: the
        # staging root, whose entries are all directories, which reports "no ELF
        # objects" for a tree full of binaries.
        include(GNUInstallDirs)
        set(_bindir_needle "_staged}/${CMAKE_INSTALL_BINDIR}/*")
        string(FIND "${_bundle_code}" "${_bindir_needle}" _bindir_position)
        if(_bindir_position EQUAL -1)
            message(STATUS "FAIL the staged glob does not name '${CMAKE_INSTALL_BINDIR}'; "
                           "CMAKE_INSTALL_BINDIR is empty where the packaging module reads it")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        else()
            message(STATUS "ok   the staged glob names ${CMAKE_INSTALL_BINDIR}")
        endif()
        # --- `file(RPATH_SET)` has to parse with this CMake ------------------
        # The text needles above only prove that the keywords are written down.
        # `RPATH_SET` is not in the documented `file()` list any more -- it is a
        # leftover from the 2.6 era that still works in 4.2 -- so its signature
        # is a fact about the interpreter, and a call shape the gate accepts can
        # still abort CPack with "unrecognized argument" (which is what the first
        # version of this script did, with the positional form).
        #
        # The call is therefore lifted out of the script under test and executed
        # for real against a non-ELF file. A probe that retyped the signature
        # here would only prove that this gate knows the syntax, never that the
        # script does: mutating the script left such a probe green.
        #
        # Only the *rejection* is read: "unrecognized argument" means the
        # signature is wrong, "does not recognize sub-command" means this CMake
        # no longer has it, and anything else means the call parsed and merely
        # failed on the payload -- a text file is not an ELF object.
        set(_rpath_probe_dir "${CMAKE_BINARY_DIR}/rpath-probe")
        file(MAKE_DIRECTORY "${_rpath_probe_dir}")
        file(WRITE "${_rpath_probe_dir}/notelf.bin" "not an ELF file\n")
        string(REGEX MATCH "file\\(RPATH_SET[^)]*\\)" _rpath_call "${_bundle_code}")
        # The call lives inside a `file(WRITE ...)` string there, so it carries the
        # escapes of the string it is written into. Undo them before executing it,
        # otherwise the probe tests its own escaping instead of the signature.
        string(REPLACE "\\\"" "\"" _rpath_call "${_rpath_call}")
        string(REPLACE "\\$" "$" _rpath_call "${_rpath_call}")
        if(NOT _rpath_call)
            message(STATUS "FAIL the bundle script has no file(RPATH_SET ...) call to probe")
            math(EXPR _snow_failures "${_snow_failures} + 1")
        else()
            file(WRITE "${_rpath_probe_dir}/probe.cmake"
                 "set(RPATH_FILE \"${_rpath_probe_dir}/notelf.bin\")\n${_rpath_call}\n")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -P "${_rpath_probe_dir}/probe.cmake"
                RESULT_VARIABLE _rpath_result
                OUTPUT_VARIABLE _rpath_stdout
                ERROR_VARIABLE _rpath_stderr)
            set(_rpath_output "${_rpath_stdout}${_rpath_stderr}")
            if(_rpath_output MATCHES "does not recognize sub-command")
                message(STATUS "FAIL this CMake has no file(RPATH_SET): ${_rpath_output}")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            elseif(_rpath_output MATCHES "unrecognized argument")
                message(STATUS "FAIL the script's ${_rpath_call} is not a signature "
                               "this CMake parses: ${_rpath_output}")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            else()
                message(STATUS "ok   the script's file(RPATH_SET ...) parses (probe exit "
                               "${_rpath_result})")
            endif()
        endif()
    endforeach()
endif()

# The dependency-prefix probe has to survive a non-flat layout. The first version
# looked for `<prefix>/lib/*.so*`, and the runner's prefix has the libraries in
# `lib/<triplet>/`: the search returned nothing and the configure died with "no
# dependency prefix in CMAKE_PREFIX_PATH" while `find_package(OpenCV)` was finding
# OpenCV in that very tree two lines earlier (run 36).
set(_fake_prefix "${CMAKE_BINARY_DIR}/fake-deps-prefix")
file(MAKE_DIRECTORY "${_fake_prefix}/lib/aarch64-linux-gnu")
file(WRITE "${_fake_prefix}/lib/aarch64-linux-gnu/liblunasvg.so.3" "")
set(_saved_prefix_path "${CMAKE_PREFIX_PATH}")
set(CMAKE_PREFIX_PATH "${_fake_prefix}")
snow_shot_linux_find_deps_lib_dir(_probe_dir)
snow_expect("dependency prefix probe finds lib/<triplet>"
            "${_fake_prefix}/lib/aarch64-linux-gnu" "${_probe_dir}")

# A prefix without any of the pinned libraries must yield nothing, so the caller
# fails loudly instead of pointing at an arbitrary directory.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/empty-deps-prefix/lib")
set(CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/empty-deps-prefix")
snow_shot_linux_find_deps_lib_dir(_probe_empty)
snow_expect("dependency prefix probe rejects an empty prefix" "" "${_probe_empty}")
set(CMAKE_PREFIX_PATH "${_saved_prefix_path}")

# --- 2d. Execute the generated script ---------------------------------------
# The text needles above can only prove that the calls are written down. Run 39
# was a script that *did* have a staging-root branch and still read the build
# host's `/usr/bin`, so the two properties that matter are checked by running
# the artifact:
#
#   * no DESTDIR and the packaging prefix -> refuse, never read the host;
#   * a staging tree with a file named `[`  -> one readable line.
#
# The second one is about the interpreter, not about this script: when a glob
# match is named `[`, `file(GLOB)` returns a *single* element whose value still
# contains the list separators (`list(LENGTH)` = 1), so `foreach(IN LISTS)` runs
# once with the whole string and `file(READ)` fails with `(File name too long)`
# naming no file at all -- 43 KB of paths in the CI log and no cause.
function(snow_shot_run_bundle script)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${script}"
        RESULT_VARIABLE _run_result
        OUTPUT_VARIABLE _run_stdout
        ERROR_VARIABLE _run_stderr)
    set(SNOW_SHOT_RUN_OUTPUT "${_run_stdout}${_run_stderr}" PARENT_SCOPE)
    set(SNOW_SHOT_RUN_RESULT "${_run_result}" PARENT_SCOPE)
endfunction()

snow_shot_run_bundle("${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}"
                     "-DCMAKE_INSTALL_PREFIX=${CPACK_PACKAGING_INSTALL_PREFIX}")
if(SNOW_SHOT_RUN_OUTPUT MATCHES "there is no staging tree")
    message(STATUS "ok   the bundle script refuses to read the packaging prefix as a tree")
else()
    message(STATUS "FAIL a bundle script without DESTDIR did not refuse the packaging "
                   "prefix: ${SNOW_SHOT_RUN_OUTPUT}")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# Regenerated against a prefix that has one of the pinned libraries, so the
# script gets past its dependency check and reaches the glob under test.
set(_bundle_probe_deps "${CMAKE_BINARY_DIR}/bundle-probe-deps")
file(MAKE_DIRECTORY "${_bundle_probe_deps}/lib/x86_64-linux-gnu")
file(WRITE "${_bundle_probe_deps}/lib/x86_64-linux-gnu/liblunasvg.so.3" "")
set(CMAKE_PREFIX_PATH "${_bundle_probe_deps}")
snow_shot_configure_linux_package()

set(_bundle_probe_tree "${CMAKE_BINARY_DIR}/bundle-probe-tree")
file(MAKE_DIRECTORY "${_bundle_probe_tree}/bin")
file(WRITE "${_bundle_probe_tree}/bin/aa" "not an ELF file\n")
file(WRITE "${_bundle_probe_tree}/bin/[" "not an ELF file\n")
snow_shot_run_bundle("${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}"
                     "-DSTAGED_DIR=${_bundle_probe_tree}")
if(SNOW_SHOT_RUN_OUTPUT MATCHES "a glob result is not a file")
    message(STATUS "ok   a collapsed glob result is reported in one line")
else()
    message(STATUS "FAIL a file named '[' in the staging tree was not reported readably: "
                   "${SNOW_SHOT_RUN_OUTPUT}")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# Put the script back to the state the rest of this gate inspects.
set(CMAKE_PREFIX_PATH "${_saved_prefix_path}")
snow_shot_configure_linux_package()

# The build prefix has to be named even in script mode (it is empty there), so
# that a real configure cannot silently forget it: CPackDeb turns it into `-l`.
if(NOT DEFINED CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS)
    message(STATUS "FAIL CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS is not assigned")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS is assigned")
endif()

# The AppImage module used to set its own `INSTALL_RPATH "$ORIGIN/../lib"`, which
# is the directory *above* the private one and ran last, undoing the packaging
# module's RPATH. One owner, one value.
file(READ "${_snow_repo}/cmake/SnowShotAppImage.cmake" _appimage_module)
if(_appimage_module MATCHES "INSTALL_RPATH")
    message(STATUS "FAIL the AppImage module sets INSTALL_RPATH itself; it has to ask "
                   "the packaging module for the private library directory")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the AppImage module leaves INSTALL_RPATH to the packaging module")
endif()

# The AppDir is staged by `cmake --install`, which knows nothing about CPack's
# install scripts -- so the private libraries only reach it because the target
# runs the same bundle script itself, on the prefix it just filled.
#
# Leaving that out was not hypothetical: the AppDir would hold the executable,
# the entry and the icon and not one `*.so*`, the executable's `$ORIGIN/../lib/`
# `snow-shot` RUNPATH would resolve to an empty directory, and the AppImage would
# be a file that installs and cannot start. `check-linux-appimage-build.py` asks
# for the bundled libraries for that reason, but that answer only arrives after
# a twenty-minute build.
string(REGEX REPLACE "#[^\n]*" "" _appimage_code "${_appimage_module}")
foreach(_needle
        "-DSTAGED_DIR="
        "SNOW_SHOT_LINUX_BUNDLE_SCRIPT"
        "--install")
    string(FIND "${_appimage_code}" "${_needle}" _appimage_position)
    if(_appimage_position EQUAL -1)
        message(STATUS "FAIL the AppImage target never uses ${_needle}")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    else()
        message(STATUS "ok   the AppImage target uses ${_needle}")
    endif()
endforeach()

# `cmake -E` is a fixed command table (Source/cmcmd.cxx: 63 entries on 4.4), and
# an unknown sub-command is a hard error that only appears when the target runs.
# Run 43 -- the first run that ever reached the AppImage -- ended its whole job
# on one:
#
#     CMake Error: cmake version 4.4.4
#     Usage: cmake -E <command> [arguments...]
#     ... forty lines of commands, none of them `chmod`
#
# The chain had ended with `cmake -E chmod +x AppRun` since it was written. It
# was also redundant: `-E copy` goes through `cmSystemTools::CopyFileAlways`,
# which reads `GetPermissions(source)` and applies `SetPermissions(destination)`,
# so the 100755 in the index already carried over. Checking the sub-command
# against the real table covers the next one too (`mkdir`, `chmod`, `ln` … are
# all plausible names that do not exist).
set(_snow_e_subcommands
    bin2c capabilities cat chdir compare_files copy copy_directory
    copy_directory_if_different copy_directory_if_newer copy_if_different
    copy_if_newer create_hardlink create_symlink echo echo_append env environment
    false make_directory md5sum remove remove_directory rename rm sha1sum sha224sum
    sha256sum sha384sum sha512sum sleep tar time touch touch_nocreate true)
string(REGEX MATCHALL [=[-E[" ]*[A-Za-z0-9_]+]=] _snow_e_uses "${_appimage_code}")
if(NOT _snow_e_uses)
    message(STATUS "FAIL the AppImage command chain runs nothing through cmake -E; the "
                   "staging commands have to be found by this check")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()
foreach(_snow_e_use IN LISTS _snow_e_uses)
    string(REGEX REPLACE [=[^-E[" ]*]=] "" _snow_e_sub "${_snow_e_use}")
    if(NOT _snow_e_sub IN_LIST _snow_e_subcommands)
        message(STATUS "FAIL the AppImage target runs 'cmake -E ${_snow_e_sub}'; that is "
                       "not one of the 63 sub-commands CMake provides, so the target "
                       "aborts with the usage text")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    else()
        message(STATUS "ok   cmake -E ${_snow_e_sub} is a real sub-command")
    endif()
endforeach()

# The cargo-built helpers (`snow-ocr-process`, `snow-shot-mcp`) are installed
# with `install(PROGRAMS "$<TARGET_FILE:...>")`. That copies the file verbatim,
# so `INSTALL_RPATH` never applies -- a cargo target is `IMPORTED`, and only
# `install(TARGETS)` runs CMake's RUNPATH rewrite. The runpath therefore has to
# be written by the linker, in `snow_add_rust_executable()`.
#
# A helper without a RUNPATH loads nothing from the private directory, and
# `dpkg-shlibdeps` sees no `$ORIGIN` to turn into `-l<dir>` either. Both halves
# are silent: the package builds, installs, and the helper fails at spawn time.
file(READ "${_snow_repo}/cmake/RustStaticLibrary.cmake" _rust_module)
string(REGEX REPLACE "#[^\n]*" "" _rust_code "${_rust_module}")
# The full relative path, not just its last component. With an empty
# `CMAKE_INSTALL_LIBDIR` the function returns `/snow-shot` -- an *absolute* path
# outside the install prefix -- and every consumer still "works": the RPATH ends
# up as `$ORIGIN/..//snow-shot` and the bundle script copies into
# `<staged>//snow-shot`. Only the value itself shows it.
snow_shot_linux_private_lib_dir(_gate_private)
snow_expect("private library directory" "${CMAKE_INSTALL_LIBDIR}/snow-shot" "${_gate_private}")
get_filename_component(_gate_private_name "${_gate_private}" NAME)
foreach(_needle
        "-Wl,-rpath,"
        "/${_gate_private_name}")
    string(FIND "${_rust_code}" "${_needle}" _rust_position)
    if(_rust_position EQUAL -1)
        message(STATUS "FAIL the cargo executables never get ${_needle} in their RUNPATH")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    else()
        message(STATUS "ok   the cargo executables get ${_needle} in their RUNPATH")
    endif()
endforeach()

# The bundled libraries have to be relocatable *before* they are copied.
# `cmake --install` strips the build-tree RPATH, and `file(RPATH_SET)` can only
# rewrite an entry that exists ("No valid ELF RPATH or RUNPATH entry exists in
# the file"), so the only place a RUNPATH can be added is the dependency build.
# Without it `libjxl.so.0.11` cannot find the `libhwy.so` shipped next to it:
# `DT_RUNPATH` is not inherited from the executable.
file(READ "${_snow_repo}/scripts/build-linux-deps.sh" _deps_script)
string(REGEX REPLACE "#[^\n]*" "" _deps_code "${_deps_script}")
set(_install_rpath_needle "-DCMAKE_INSTALL_RPATH=$ORIGIN")
string(FIND "${_deps_code}" "${_install_rpath_needle}" _install_rpath_position)
if(_install_rpath_position EQUAL -1)
    message(STATUS "FAIL the dependency build never sets ${_install_rpath_needle}; the "
                   "bundled libraries would have no way to find each other")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the dependency build sets CMAKE_INSTALL_RPATH")
endif()

# The same function on an arm64 host must select the arm64 package.
set(CMAKE_SYSTEM_PROCESSOR "aarch64")
snow_shot_configure_linux_package()
snow_expect("arm64 host architecture" "arm64" "${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
# Read after the call, so this also proves the path leaves the function's scope:
# the AppImage target is attached from CMakeLists.txt, not from inside it, and an
# unexported variable there expands to nothing and fails at build time with
# `cmake -P: no file given`.
snow_expect("the bundle script path leaves the packaging function"
            "${CMAKE_BINARY_DIR}/snow-shot-linux-runtime-bundle.cmake"
            "${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}")
set(CMAKE_SYSTEM_PROCESSOR "x86_64")

# --- 3. The freedesktop files are installed, not just present ----------------
foreach(_installed
        "packaging/linux/com.snowshot.snow_shot.desktop"
        "packaging/linux/com.snowshot.snow_shot.metainfo.xml"
        "resources/app-icon.svg")
    if(NOT EXISTS "${_snow_repo}/snow_shot/${_installed}")
        message(STATUS "FAIL missing packaging input: snow_shot/${_installed}")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

file(READ "${_snow_repo}/snow_shot/CMakeLists.txt" _snow_shot_cmake)
foreach(_needle
        "packaging/linux/com.snowshot.snow_shot.desktop"
        "packaging/linux/com.snowshot.snow_shot.metainfo.xml"
        "resources/app-icon.svg")
    string(FIND "${_snow_shot_cmake}" "${_needle}" _snow_position)
    if(_snow_position EQUAL -1)
        message(STATUS "FAIL snow_shot/CMakeLists.txt does not install ${_needle}")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

# --- 3b. The fixed-size hicolor icons ---------------------------------------
# The scalable SVG alone is not enough: XFCE's panel, dde-dock and the AppImage
# launchers look the icon name up in the sized hicolor directories only. The
# PNGs are generated from app-icon.ico by scripts/gen-linux-icons.py, so a
# deleted or unregenerated size has to fail here rather than at install time on
# a distribution build root that has no icon tools to fix it up.
set(_snow_hicolor_root "${_snow_repo}/snow_shot/resources/icons/hicolor")
string(FIND "${_snow_shot_cmake}" "resources/icons/hicolor" _snow_position)
if(_snow_position EQUAL -1)
    message(STATUS "FAIL snow_shot/CMakeLists.txt does not install the hicolor PNG icons")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()
foreach(_size 16 24 32 48 64 128 256)
    set(_snow_png "${_snow_hicolor_root}/${_size}x${_size}/apps/snow-shot.png")
    if(NOT EXISTS "${_snow_png}")
        message(STATUS "FAIL missing hicolor icon snow_shot/resources/icons/hicolor/${_size}x${_size}/apps/snow-shot.png")
        math(EXPR _snow_failures "${_snow_failures} + 1")
        continue()
    endif()
    # An .ico entry copied out of place, or a file that a text editor wrote
    # over, would still exists() -- the signature is what makes it a PNG.
    file(READ "${_snow_png}" _snow_png_signature HEX LIMIT 8)
    if(NOT _snow_png_signature STREQUAL "89504e470d0a1a0a")
        message(STATUS "FAIL ${_size}x${_size}/apps/snow-shot.png is not a PNG")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

# --- 4. The desktop entry and the package agree on the application id -------
file(READ "${_snow_repo}/snow_shot/packaging/linux/com.snowshot.snow_shot.desktop" _snow_desktop)
foreach(_required
        "Type=Application"
        "Exec=snow_shot"
        "TryExec=snow_shot"
        "Icon=snow-shot"
        "Categories=Graphics;"
        "StartupWMClass=snow_shot")
    string(FIND "${_snow_desktop}" "${_required}" _snow_position)
    if(_snow_position EQUAL -1)
        message(STATUS "FAIL com.snowshot.snow_shot.desktop is missing '${_required}'")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

# The package must not disagree with the source tree about its own version.
file(READ "${_snow_repo}/snow_shot/packaging/linux/com.snowshot.snow_shot.metainfo.xml" _snow_metainfo)
string(FIND "${_snow_metainfo}" "<id>com.snowshot.snow_shot</id>" _snow_position)
if(_snow_position EQUAL -1)
    message(STATUS "FAIL com.snowshot.snow_shot.metainfo.xml does not carry the reverse-DNS id")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# --- 5. The AppImage layout --------------------------------------------------
# `cmake/SnowShotAppImage.cmake` attaches a target and calls appimagetool, so on
# this host it is never parsed by a real configure. The two pure functions in it
# are the ones that carry the decisions worth testing, and both can run here.
include("${_snow_repo}/cmake/SnowShotAppImage.cmake")

snow_shot_appimage_architecture(_appimage_arch)
snow_expect("host appimage architecture" "x86_64" "${_appimage_arch}")

foreach(_case
        "x86_64:x86_64"
        "AMD64:x86_64"
        "aarch64:aarch64"
        "arm64:aarch64")
    string(REPLACE ":" ";" _pair "${_case}")
    list(GET _pair 0 _processor)
    list(GET _pair 1 _expected)
    snow_shot_appimage_architecture(_mapped PROCESSOR "${_processor}")
    snow_expect("appimage ${_processor}" "${_expected}" "${_mapped}")
endforeach()

# AppImage has no runtime for these two, and a silent wrong answer would be a
# bundle that no kernel executes.
foreach(_processor sparc64 loongarch64 riscv64)
    # CMAKE_BINARY_DIR is deliberately a non-existent path here, so the scratch
    # file goes where script mode actually has write access.
    set(_negative_script "${CMAKE_CURRENT_BINARY_DIR}/check-linux-appimage-negative-${_processor}.cmake")
    if(NOT CMAKE_CURRENT_BINARY_DIR)
        set(_negative_script "${CMAKE_BINARY_DIR}/cmake/check-linux-appimage-negative-${_processor}.cmake")
    endif()
    file(WRITE "${_negative_script}"
        "include(\"${_snow_repo}/cmake/SnowShotAppImage.cmake\")\n\
snow_shot_appimage_architecture(_arch PROCESSOR \"${_processor}\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${_negative_script}"
        RESULT_VARIABLE _negative_result
        OUTPUT_QUIET
        ERROR_QUIET)
    file(REMOVE "${_negative_script}")
    if(_negative_result EQUAL 0)
        message(STATUS "FAIL appimage ${_processor}: accepted, but no runtime exists for it")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    else()
        message(STATUS "ok   appimage ${_processor} rejected (exit ${_negative_result})")
    endif()
endforeach()

snow_shot_appimage_layout(APP_SOURCE_DIR "${_snow_repo}/snow_shot")
snow_expect("appimage file name" "snow-shot-1.2.8-x86_64.AppImage"
            "${SNOW_SHOT_APPIMAGE_FILE_NAME}")
snow_expect("appimage install prefix" "${CMAKE_BINARY_DIR}/appimage/snow_shot-1.2.8-x86_64.AppDir/usr"
            "${SNOW_SHOT_APPIMAGE_USR}")
# appimagetool reads the entry and the icon from the AppDir root, and the icon
# has to be named exactly what Icon= says.
snow_expect("appimage root icon name" "snow-shot.png" "${SNOW_SHOT_APPIMAGE_ICON_NAME}")

# The entry exists under three names by the time the AppImage is built: the file
# in the source tree, the install rule's copy under `usr/share/applications`, and
# the AppDir root copy that appimagetool reads. The layout function answers with
# one name, so both consumers use it; these two checks tie that name back to a
# file that actually exists and to the place the install rules put it.
snow_expect("appimage installed entry"
            "${CMAKE_BINARY_DIR}/appimage/snow_shot-1.2.8-x86_64.AppDir/usr/share/applications/com.snowshot.snow_shot.desktop"
            "${SNOW_SHOT_APPIMAGE_DESKTOP}")
if(EXISTS "${_snow_repo}/snow_shot/packaging/linux/${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}")
    message(STATUS "ok   appimage entry source is packaging/linux/${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}")
else()
    message(STATUS "FAIL appimage entry source does not exist: "
                   "packaging/linux/${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# The AppDir root copy has to carry the same name as the installed entry, and the
# only way that stays true is if the command goes through the layout variable.
# A second literal would still build an AppImage -- and fail `appstreamcli` on a
# machine with no toolchain to rebuild it. So: the file defines the name once.
file(READ "${_snow_repo}/cmake/SnowShotAppImage.cmake" _snow_appimage_cmake)
string(REGEX MATCHALL "[A-Za-z0-9_.-]+\\.desktop" _snow_entry_literals "${_snow_appimage_cmake}")
list(LENGTH _snow_entry_literals _snow_entry_literal_count)
if(_snow_entry_literal_count EQUAL 1)
    message(STATUS "ok   the AppImage entry name is written once")
else()
    message(STATUS "FAIL cmake/SnowShotAppImage.cmake writes the entry name "
                   "${_snow_entry_literal_count} time(s) (${_snow_entry_literals}); it has to be "
                   "defined once and used through SNOW_SHOT_APPIMAGE_DESKTOP_NAME")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()
string(FIND "${_snow_appimage_cmake}" "\${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}" _snow_position)
if(_snow_position EQUAL -1)
    message(STATUS "FAIL the AppDir root copy does not use SNOW_SHOT_APPIMAGE_DESKTOP_NAME")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   the AppDir root copy uses the layout name")
endif()

# The two files staged into the AppDir have to exist where the layout says they
# are. They used to be built from `${CMAKE_CURRENT_SOURCE_DIR}` inside a function
# the *root* CMakeLists calls, so both resolved to `<repo>/packaging/...` and
# `<repo>/resources/...`; nothing caught it, and the configure that turned the
# option on reported a missing icon that was sitting under `snow_shot/` all
# along. This check is the one that would have said so.
foreach(_staged APPRUN_SOURCE ICON_SOURCE)
    if(EXISTS "${SNOW_SHOT_APPIMAGE_${_staged}}")
        message(STATUS "ok   appimage ${_staged} exists")
    else()
        message(STATUS "FAIL appimage ${_staged} does not exist: "
                       "${SNOW_SHOT_APPIMAGE_${_staged}}")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

file(READ "${_snow_repo}/CMakeLists.txt" _snow_root_cmake)
string(FIND "${_snow_root_cmake}" "snow_shot_add_appimage_target()" _snow_position)
if(_snow_position EQUAL -1)
    message(STATUS "FAIL CMakeLists.txt never calls snow_shot_add_appimage_target()")
    math(EXPR _snow_failures "${_snow_failures} + 1")
endif()

# The scratch tree is only an input to the checks above; leaving it behind turns
# a green run into an untracked directory in the repository root.
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")

if(_snow_failures GREATER 0)
    message(FATAL_ERROR "Linux packaging check failed: ${_snow_failures} problem(s)")
endif()
message(STATUS "Linux packaging check passed")
