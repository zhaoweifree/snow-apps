# Checks cmake/SnowQt5InstallPaths.cmake against both Qt5 kit layouts.
#
# Why this exists: the two locations it resolves (the tools directory holding
# `lrelease`, and the translations directory holding `qtbase_*.qm`) are wrong on
# a Debian/Ubuntu multiarch kit if they are derived the prefix-shaped way --
# `Qt5Core_DIR` there is /usr/lib/<triple>/cmake/Qt5Core, so `../../../bin` is
# /usr/lib/bin and `../../../translations` is /usr/lib/translations. Neither
# exists. The failure is invisible on this workstation (its Qt is a custom
# prefix kit) and only showed up in the first real Linux configure (§9.6.8).
#
# This script builds both layouts in a temporary directory and asserts the
# resolver picks the right one for each, so a regression fails here -- on
# Windows, in the same `scripts/check-linux-packaging.sh` run -- instead of
# three CI runs later.
#
# Usage: cmake -P scripts/check-qt5-install-paths.cmake

set(_root "${CMAKE_CURRENT_BINARY_DIR}/check-qt5-install-paths")
file(REMOVE_RECURSE "${_root}")

set(_failures 0)

# `actual` and `expected` are *values*. An earlier version dereferenced them a
# second time (`${${actual}}`), which turns any path into the empty string --
# and a gate that compares "" with "" passes no matter what the resolver does.
function(_check label actual expected)
    if("${actual}" STREQUAL "${expected}")
        message(STATUS "ok   ${label}")
    else()
        message(SEND_ERROR "FAIL ${label}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(_make_layout root kind)
    if(kind STREQUAL "multiarch")
        # Debian/Ubuntu: /usr/lib/<triple>/cmake/Qt5Core + /usr/share/qt5/translations
        file(MAKE_DIRECTORY "${root}/usr/lib/x86_64-linux-gnu/cmake/Qt5Core")
        file(MAKE_DIRECTORY "${root}/usr/share/qt5/translations")
        file(MAKE_DIRECTORY "${root}/usr/bin")
        file(WRITE "${root}/usr/bin/lrelease" "")
    else()
        # Custom prefix kit: <prefix>/lib/cmake/Qt5Core + <prefix>/translations
        file(MAKE_DIRECTORY "${root}/lib/cmake/Qt5Core")
        file(MAKE_DIRECTORY "${root}/translations")
        file(MAKE_DIRECTORY "${root}/bin")
        file(WRITE "${root}/bin/lrelease" "")
    endif()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SnowQt5InstallPaths.cmake")

# Both resolvers ask qmake first. The query is the *best* answer when the kit
# has one, but this check is about the derivation that runs when it does not,
# so the query is stubbed out to an empty program.
set(SNOW_QT5_QMAKE_PROGRAM "")

_make_layout("${_root}/multiarch" multiarch)
_make_layout("${_root}/prefix" prefix)

set(Qt5Core_DIR "${_root}/multiarch/usr/lib/x86_64-linux-gnu/cmake/Qt5Core")
snow_qt5_tools_dir(_tools)
snow_qt5_translations_dir(_translations)
_check("multiarch: lrelease directory"
    "${_tools}" "${_root}/multiarch/usr/bin")
_check("multiarch: translations directory"
    "${_translations}" "${_root}/multiarch/usr/share/qt5/translations")

set(Qt5Core_DIR "${_root}/prefix/lib/cmake/Qt5Core")
snow_qt5_tools_dir(_tools)
snow_qt5_translations_dir(_translations)
_check("prefix kit: lrelease directory"
    "${_tools}" "${_root}/prefix/bin")
_check("prefix kit: translations directory"
    "${_translations}" "${_root}/prefix/translations")

# An explicit override always wins, even when nothing is on disk: the point of
# the override is to be able to say "it is here" without the layout agreeing.
set(SNOW_QT_BIN_DIR "${_root}/nowhere/bin")
snow_qt5_tools_dir(_tools)
_check("override: tools directory is taken as given"
    "${_tools}" "${_root}/nowhere/bin")
unset(SNOW_QT_BIN_DIR)

# With no discoverable layout at all the resolver must return empty, not the
# first guess -- callers turn that into a message naming the override.
set(SNOW_QT5_QMAKE_PROGRAM "")
set(Qt5Core_DIR "")
set(QT5_INSTALL_PREFIX "")
set(SNOW_QT_BIN_DIR "${_root}/nowhere/bin")
snow_qt5_translations_dir(_translations)
_check("no layout: translations resolve to empty"
    "${_translations}" "")
unset(SNOW_QT_BIN_DIR)

file(REMOVE_RECURSE "${_root}")
message(STATUS "Qt5 install path check completed")
