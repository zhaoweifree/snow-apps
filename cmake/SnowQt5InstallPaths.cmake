# Resolve the two Qt 5 install locations that are *outside* `<prefix>/lib` on a
# Debian/Ubuntu-style multiarch kit.
#
# Background: on a custom Qt kit `Qt5Core_DIR` is `<prefix>/lib/cmake/Qt5Core`,
# so `../../../bin` really is `<prefix>/bin`. A distribution kit puts the same
# file in `/usr/lib/<triple>/cmake/Qt5Core`, where `../../../bin` resolves to
# `/usr/lib/bin` -- a directory that does not exist on any Debian derivative.
# The translations live at `/usr/share/qt5/translations`, two levels away from
# that derivation as well. Both mistakes are invisible until something actually
# needs `lrelease` or a stock catalog, which is why they survived until the
# first real Linux configure (docs-uos-linux-port.md §9.6.8).
#
# Resolution order, deliberately the same for both:
#   1. the explicit cache override (`SNOW_QT_BIN_DIR` / `SNOW_QT5_TRANSLATIONS_DIR`);
#   2. the shape derived from `Qt5Core_DIR`, which comes from the Qt5 package
#      that was actually found -- prefix-shaped first, multiarch-shaped second;
#   3. `qmake -query`, the kit's own answer, used only when both shapes are
#      absent (a qmake on PATH may belong to a different Qt, so it must never
#      outrank a path derived from the kit actually in use).
# A candidate is only accepted if it exists on disk, so a wrong guess moves on
# to the next one instead of failing much later with a misleading message.
include_guard(GLOBAL)

function(_snow_qt5_qmake outvar)
    if(DEFINED SNOW_QT5_QMAKE_PROGRAM)
        set(${outvar} "${SNOW_QT5_QMAKE_PROGRAM}" PARENT_SCOPE)
        return()
    endif()
    find_program(_snow_qt5_qmake_path NAMES qmake qmake-qt5)
    set(SNOW_QT5_QMAKE_PROGRAM "${_snow_qt5_qmake_path}" CACHE FILEPATH
        "qmake used to query the Qt5 install layout (empty if none)")
    unset(_snow_qt5_qmake_path)
    set(${outvar} "${SNOW_QT5_QMAKE_PROGRAM}" PARENT_SCOPE)
endfunction()

function(snow_qt5_query_path outvar query)
    # Cross-compilation caveat: `qmake -query` answers for the kit it belongs
    # to, which for a cross kit is the target. That is what the build wants, so
    # no special case is needed -- but the qmake must be the kit's own.
    _snow_qt5_qmake(_snow_qmake)
    set(_snow_value "")
    if(_snow_qmake)
        execute_process(
            COMMAND "${_snow_qmake}" -query "${query}"
            OUTPUT_VARIABLE _snow_value
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _snow_result
            ERROR_QUIET)
        if(NOT _snow_result EQUAL 0)
            set(_snow_value "")
        endif()
    endif()
    set(${outvar} "${_snow_value}" PARENT_SCOPE)
endfunction()

function(snow_qt5_tools_dir outvar)
    if(SNOW_QT_BIN_DIR)
        set(${outvar} "${SNOW_QT_BIN_DIR}" PARENT_SCOPE)
        return()
    endif()

    set(_candidates)
    if(Qt5Core_DIR)
        get_filename_component(_prefix_shaped "${Qt5Core_DIR}/../../../bin" ABSOLUTE)
        get_filename_component(_multiarch_shaped "${Qt5Core_DIR}/../../../../bin" ABSOLUTE)
        list(APPEND _candidates "${_prefix_shaped}" "${_multiarch_shaped}")
    endif()
    if(QT5_INSTALL_PREFIX)
        list(APPEND _candidates "${QT5_INSTALL_PREFIX}/bin")
    endif()
    snow_qt5_query_path(_query_bins QT_INSTALL_BINS)
    if(_query_bins)
        list(APPEND _candidates "${_query_bins}")
    endif()

    # `lrelease` is the tool this directory is actually used for; accepting a
    # directory that merely exists would push the failure to the first build
    # that invokes the tool.
    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}/lrelease")
            set(${outvar} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${outvar} "" PARENT_SCOPE)
endfunction()

function(snow_qt5_translations_dir outvar)
    if(SNOW_QT5_TRANSLATIONS_DIR)
        set(${outvar} "${SNOW_QT5_TRANSLATIONS_DIR}" PARENT_SCOPE)
        return()
    endif()

    set(_candidates)
    if(Qt5Core_DIR)
        get_filename_component(_prefix_shaped "${Qt5Core_DIR}/../../../translations" ABSOLUTE)
        get_filename_component(_multiarch_shaped
            "${Qt5Core_DIR}/../../../../share/qt5/translations" ABSOLUTE)
        list(APPEND _candidates "${_prefix_shaped}" "${_multiarch_shaped}")
    endif()
    if(SNOW_QT_BIN_DIR)
        get_filename_component(_beside_bin "${SNOW_QT_BIN_DIR}/../translations" ABSOLUTE)
        get_filename_component(_share_beside_bin
            "${SNOW_QT_BIN_DIR}/../share/qt5/translations" ABSOLUTE)
        list(APPEND _candidates "${_beside_bin}" "${_share_beside_bin}")
    endif()
    snow_qt5_query_path(_query_translations QT_INSTALL_TRANSLATIONS)
    if(_query_translations)
        list(APPEND _candidates "${_query_translations}")
    endif()

    foreach(_candidate IN LISTS _candidates)
        if(IS_DIRECTORY "${_candidate}")
            set(${outvar} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${outvar} "" PARENT_SCOPE)
endfunction()
