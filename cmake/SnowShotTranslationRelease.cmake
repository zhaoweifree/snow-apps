include_guard(GLOBAL)

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
        # Resolution order: explicit override, then derive from the resolved kit
        # (Qt5Core_DIR is <prefix>/lib/cmake/Qt5Core), then from the kit bin dir.
        if(SNOW_QT5_TRANSLATIONS_DIR)
            set(_qt_translation_dir "${SNOW_QT5_TRANSLATIONS_DIR}")
        elseif(Qt5Core_DIR)
            get_filename_component(_qt_translation_dir
                "${Qt5Core_DIR}/../../../translations" ABSOLUTE)
        elseif(SNOW_QT_BIN_DIR)
            get_filename_component(_qt_translation_dir
                "${SNOW_QT_BIN_DIR}/../translations" ABSOLUTE)
        else()
            message(FATAL_ERROR
                "Cannot locate the Qt5 translations directory: Qt5Core_DIR and "
                "SNOW_QT_BIN_DIR are both unset. Set SNOW_QT5_TRANSLATIONS_DIR explicitly.")
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
    qt_add_lrelease(
        TS_FILES "${catalog_dir}/snow_shot_en_US.ts"
        LRELEASE_TARGET ${target}_source_language
        QM_FILES_OUTPUT_VARIABLE _source_qm_files
        QM_OUTPUT_DIRECTORY "${output_dir}"
        OPTIONS -fail-on-unfinished)
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
