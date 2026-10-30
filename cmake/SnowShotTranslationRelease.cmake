include_guard(GLOBAL)

# `CMAKE_CURRENT_LIST_DIR` 在函数体内求值的是**调用方**的目录，不是本文件目录。
# snow_shot 调本函数时它指向 snow_shot/，于是 include 会去找
# snow_shot/SnowQt5InstallPaths.cmake 并失败。路径在文件作用域固化。
set(SNOW_QT5_INSTALL_PATHS_MODULE "${CMAKE_CURRENT_LIST_DIR}/SnowQt5InstallPaths.cmake")

function(snow_shot_release_translation_catalogs target catalog_dir output_dir output_variable)
    # Qt 6 exports the directory through its own variables; Qt 5.15 exposes neither
    # `QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR` nor `QT6_INSTALL_*`, so without the
    # version split below the path degrades to "/" and the stock-catalog check
    # aborts configuration on the Qt5 route.
    #
    # The Qt6 branch is the *else* of "SNOW_QT_MAJOR is 5" on purpose: this file is
    # also included by isolated CMake fixtures that do not load cmake/SnowQt.cmake,
    # and they must keep resolving exactly as before the Qt5 route existed.
    if(SNOW_QT_MAJOR STREQUAL "5")
        # Resolution lives in SnowQt5InstallPaths.cmake: a Debian/Ubuntu kit keeps
        # Qt5Core_DIR under /usr/lib/<triple>/cmake, where the prefix-shaped
        # derivation below lands on /usr/lib/translations (which does not exist)
        # while the catalogs are in /usr/share/qt5/translations. That module asks
        # qmake first and only then tries both shapes.
        include("${SNOW_QT5_INSTALL_PATHS_MODULE}")
        if(SNOW_QT5_TRANSLATIONS_DIR)
            set(_qt_translation_dir "${SNOW_QT5_TRANSLATIONS_DIR}")
        else()
            snow_qt5_translations_dir(_qt_translation_dir)
        endif()
        if(NOT _qt_translation_dir)
            message(FATAL_ERROR
                "Cannot locate the Qt5 translations directory: neither qmake -query "
                "QT_INSTALL_TRANSLATIONS nor any derivation from Qt5Core_DIR / "
                "SNOW_QT_BIN_DIR produced an existing directory. "
                "Set SNOW_QT5_TRANSLATIONS_DIR explicitly.")
        endif()
    else()
        set(_qt_translation_dir "${QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR}")
        if(_qt_translation_dir STREQUAL "")
            set(_qt_translation_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
        endif()
    endif()
    if(SNOW_QT_MAJOR STREQUAL "5")
        # Qt 5.15 splits its Chinese stock-dialog catalogs differently from Qt 6 and
        # its kit layout must not be assumed: the official 5.15.2 kit ships
        # `qtbase_zh_TW.qm` but **no** `qtbase_zh_CN.qm`, because Simplified Chinese
        # is only published there as the aggregate `qt_zh_CN.qm`. Qt5 also has no
        # catalog merging (see the qt_add_lrelease shim), so these files are loaded
        # at runtime / shipped by the packaging step instead of being merged into
        # snow_shot_<locale>.qm. The guard therefore accepts any of the kit's own
        # names for each locale and fails only when *no* Chinese catalog exists.
        set(_qt5_simplified_catalogs "qtbase_zh_CN.qm" "qt_zh_CN.qm")
        set(_qt5_traditional_catalogs "qtbase_zh_TW.qm" "qt_zh_TW.qm")
        foreach(_locale IN ITEMS zh_CN zh_TW)
            if(_locale STREQUAL "zh_CN")
                set(_catalogs "${_qt5_simplified_catalogs}")
            else()
                set(_catalogs "${_qt5_traditional_catalogs}")
            endif()
            set(_found FALSE)
            foreach(_candidate IN LISTS _catalogs)
                if(EXISTS "${_qt_translation_dir}/${_candidate}")
                    set(_found TRUE)
                    break()
                endif()
            endforeach()
            if(NOT _found)
                message(FATAL_ERROR
                    "The selected Qt5 kit is missing a stock-dialog catalog for "
                    "${_locale} under ${_qt_translation_dir}. The kit may name it "
                    "either by the Qt6 spelling or by its own aggregate name; install "
                    "the qttranslations module for Qt 5.15.")
            endif()
        endforeach()
        unset(_catalogs)
        unset(_candidate)
        unset(_found)
        unset(_qt5_simplified_catalogs)
        unset(_qt5_traditional_catalogs)
        unset(_locale)
    else()
        foreach(_locale IN ITEMS zh_CN zh_TW)
            if(NOT EXISTS "${_qt_translation_dir}/qtbase_${_locale}.qm")
                message(FATAL_ERROR
                    "The selected Qt kit is missing qtbase_${_locale}.qm. "
                    "Build/install the qttranslations module to preserve stock-dialog translations.")
            endif()
        endforeach()
        unset(_locale)
    endif()

    # Qt widgets already use English source text. Merging an empty qtbase_en
    # catalog into en_US adds no strings and causes a misleading missing-catalog warning.
    # Source-identical messages fall back to English without occupying the embedded
    # catalog. Keep real translations, including language metadata and plural forms.
    qt_add_lrelease(
        TS_FILES "${catalog_dir}/snow_shot_en_US.ts"
        LRELEASE_TARGET ${target}_source_language
        QM_FILES_OUTPUT_VARIABLE _source_qm_files
        QM_OUTPUT_DIRECTORY "${output_dir}"
        OPTIONS -fail-on-unfinished -removeidentical)
    qt_add_lrelease(
        TS_FILES "${catalog_dir}/snow_shot_zh_CN.ts" "${catalog_dir}/snow_shot_zh_TW.ts"
        LRELEASE_TARGET ${target}_localized
        QM_FILES_OUTPUT_VARIABLE _localized_qm_files
        QM_OUTPUT_DIRECTORY "${output_dir}"
        MERGE_QT_TRANSLATIONS
        QT_TRANSLATION_CATALOGS qtbase
        OPTIONS -fail-on-unfinished)
    add_custom_target(${target} ALL)
    add_dependencies(${target} ${target}_source_language ${target}_localized)
    set(${output_variable} ${_source_qm_files} ${_localized_qm_files} PARENT_SCOPE)
endfunction()
