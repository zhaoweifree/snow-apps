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
if(NOT CPACK_INSTALL_SCRIPTS)
    message(STATUS "FAIL CPACK_INSTALL_SCRIPTS is empty: nothing would bundle the "
                   "self-built libraries, and dpkg-shlibdeps fails on the first "
                   "one it cannot find")
    math(EXPR _snow_failures "${_snow_failures} + 1")
else()
    message(STATUS "ok   CPACK_INSTALL_SCRIPTS names the bundle script")
    foreach(_script IN LISTS CPACK_INSTALL_SCRIPTS)
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
        foreach(_needle
                "GET_RUNTIME_DEPENDENCIES"
                "CPACK_TEMPORARY_DIRECTORY"
                "RPATH_SET")
            if(NOT _bundle_code MATCHES "${_needle}")
                message(STATUS "FAIL the bundle script never uses ${_needle}")
                math(EXPR _snow_failures "${_snow_failures} + 1")
            else()
                message(STATUS "ok   the bundle script uses ${_needle}")
            endif()
        endforeach()
    endforeach()
endif()

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

# The same function on an arm64 host must select the arm64 package.
set(CMAKE_SYSTEM_PROCESSOR "aarch64")
snow_shot_configure_linux_package()
snow_expect("arm64 host architecture" "arm64" "${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
set(CMAKE_SYSTEM_PROCESSOR "x86_64")

# --- 3. The freedesktop files are installed, not just present ----------------
foreach(_installed
        "packaging/linux/snow-shot.desktop"
        "packaging/linux/snow-shot.metainfo.xml"
        "resources/app-icon.svg")
    if(NOT EXISTS "${_snow_repo}/snow_shot/${_installed}")
        message(STATUS "FAIL missing packaging input: snow_shot/${_installed}")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

file(READ "${_snow_repo}/snow_shot/CMakeLists.txt" _snow_shot_cmake)
foreach(_needle
        "packaging/linux/snow-shot.desktop"
        "packaging/linux/snow-shot.metainfo.xml"
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
file(READ "${_snow_repo}/snow_shot/packaging/linux/snow-shot.desktop" _snow_desktop)
foreach(_required
        "Type=Application"
        "Exec=snow_shot"
        "TryExec=snow_shot"
        "Icon=snow-shot"
        "Categories=Graphics;"
        "StartupWMClass=snow_shot")
    string(FIND "${_snow_desktop}" "${_required}" _snow_position)
    if(_snow_position EQUAL -1)
        message(STATUS "FAIL snow-shot.desktop is missing '${_required}'")
        math(EXPR _snow_failures "${_snow_failures} + 1")
    endif()
endforeach()

# The package must not disagree with the source tree about its own version.
file(READ "${_snow_repo}/snow_shot/packaging/linux/snow-shot.metainfo.xml" _snow_metainfo)
string(FIND "${_snow_metainfo}" "<id>snow-shot.desktop</id>" _snow_position)
if(_snow_position EQUAL -1)
    message(STATUS "FAIL snow-shot.metainfo.xml does not launch snow-shot.desktop")
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
