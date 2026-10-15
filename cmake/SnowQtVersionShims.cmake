# Snow Apps —— Qt 5 / Qt 6 版本无关命令封装
#
# 目的：让 CMake 脚本里沿用 Qt6 风格的 `qt_*` 命令名，在 Qt 5.15 下也能工作，
#       从而避免在 11 种命令、83 处调用点上散布版本分支。
#
# 依据（实仓 grep，见 docs-qt5-downgrade-plan.md §1.1 表 B）：
#   qt_import_plugins(61) qt_add_resources(8) qt_add_executable(3)
#   qt_generate_deploy_app_script(2) qt_finalize_executable(2) qt_add_lrelease(2)
#   qt_standard_project_setup(1) qt_add_translations(1) qt_add_lupdate(1) qt_add_library(1)
#
# 用法：由 cmake/SnowQt.cmake 的 snow_find_qt() 在 Qt5 分支中 include 一次。
#
# 设计原则：
#   1. 只在 Qt5 下补齐缺失命令；Qt6 下本文件整体 return。
#   2. 一律用 `if(NOT COMMAND ...)` 守卫，绝不覆盖 Qt 自带的 versionless 命令。
#   3. 不做静默降级：无法等价实现的命令显式打印 STATUS/WARNING，
#      避免"配置通过但产物不对"。
#
# 与 Qt6 的语义差异已逐条核对本地 Qt 5.15.2 套件（不是照文档推测）：
#   * Qt 5.15 **只对模块**建立 versionless 目标（Qt::Core / Qt::Gui …），
#     插件目标没有 Qt:: 别名，只有 Qt5::<Plugin>。
#     → 因此 R1b 规则把 `Qt6::<插件>` 映射为 `${SNOW_QT_NS}::<插件>`，
#       而不是 `Qt::<插件>`。（已实测 Qt5 导出 QWindowsIntegrationPlugin、
#       QOffscreenIntegrationPlugin、QICOPlugin、QSvgPlugin、QJpegPlugin、QGifPlugin。）
#   * Qt5 **不导出 BundledLibjpeg**，snow_image 的静态 JPEG 分支需版本分支处理。
#   * Qt5 的 QKeySequence 无公开可变接口（setKey 是 private，operator[] 返回 int 右值），
#     但提供 4 参构造 —— 见 snow_shot/include/snow_shot/compat/qt_compat.h。

include_guard(GLOBAL)

if(NOT DEFINED Qt5_FOUND AND NOT DEFINED Qt6_FOUND)
    message(FATAL_ERROR
        "SnowQtVersionShims.cmake must be included after a Qt package has been found. "
        "Resolve Qt with snow_find_qt() first.")
endif()

if(DEFINED Qt6_FOUND AND NOT DEFINED Qt5_FOUND)
    # Qt6：官方已提供全部所需命令，无需封装。
    message(STATUS "Snow Qt shims: Qt6 detected, versionless shims not required.")
    return()
endif()

message(STATUS "Snow Qt shims: Qt5 detected (${Qt5_VERSION}), installing Qt6-compatible command shims.")

# ---------------------------------------------------------------------------
# 解析当前 Qt 套件的 bin 目录（lupdate/lrelease/windeployqt 所在处）。
# 结果缓存到 SNOW_QT_SHIM_BIN_DIR，避免每次重复推断。
# ---------------------------------------------------------------------------
function(_snow_qt_shim_bin_dir outvar)
    if(SNOW_QT_SHIM_BIN_DIR)
        set(${outvar} "${SNOW_QT_SHIM_BIN_DIR}" PARENT_SCOPE)
        return()
    endif()
    set(_bin "")
    if(SNOW_QT_BIN_DIR)
        set(_bin "${SNOW_QT_BIN_DIR}")
    elseif(Qt5Core_DIR)
        get_filename_component(_bin "${Qt5Core_DIR}/../../../bin" ABSOLUTE)
    elseif(QT5_INSTALL_PREFIX)
        set(_bin "${QT5_INSTALL_PREFIX}/bin")
    endif()
    set(SNOW_QT_SHIM_BIN_DIR "${_bin}" CACHE INTERNAL "Qt5 kit bin directory (shim)" FORCE)
    set(${outvar} "${_bin}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Qt6 → Qt5 的 lrelease 选项翻译
#
# Qt6 的 lrelease 有 `-fail-on-unfinished`；Qt 5.15.2 的 lrelease **没有**
# （实测 `lrelease -help` 只列出 -nounfinished / -removeidentical / ...）。
# 原样透传会让 lrelease 打印用法并以非零码退出。把开关从 Qt5 命令行摘掉，
# 改为交给 SnowQt5LreleaseEnforce.cmake 做等价校验——门禁不能因为换版本而消失，
# 静默降级（残缺目录照常打包）比构建失败更糟。
#
# 输出：options_out = 过滤后的选项；enforce_out = 是否需要对每个 .ts 追加完整性校验。
# ---------------------------------------------------------------------------
set(SNOW_QT_LRELEASE_ENFORCE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/SnowQt5LreleaseEnforce.cmake")

function(_snow_qt_translate_lrelease_options options_out enforce_out in_options)
    set(_snow_filtered ${in_options})
    set(_snow_enforce FALSE)
    if("-fail-on-unfinished" IN_LIST _snow_filtered)
        list(REMOVE_ITEM _snow_filtered "-fail-on-unfinished")
        set(_snow_enforce TRUE)
    endif()
    set(${options_out} "${_snow_filtered}" PARENT_SCOPE)
    set(${enforce_out} "${_snow_enforce}" PARENT_SCOPE)
endfunction()

# 生成"跑一个 .ts"的 COMMAND 片段（需要完整性校验时走脚本模式包装）。
function(_snow_qt_lrelease_command out_commands enforce lrelease ts_file qm_file options)
    if(enforce)
        set(${out_commands}
            COMMAND "${CMAKE_COMMAND}"
                    "-DSNOW_LRELEASE=${lrelease}"
                    "-DSNOW_TS_FILE=${ts_file}"
                    "-DSNOW_QM_FILE=${qm_file}"
                    "-DSNOW_LRELEASE_OPTIONS=${options}"
                    -P "${SNOW_QT_LRELEASE_ENFORCE_SCRIPT}"
            PARENT_SCOPE)
    else()
        set(${out_commands}
            COMMAND "${lrelease}" ${options} "${ts_file}" -qm "${qm_file}"
            PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 资源编译
#
# Qt6 的 qt_add_resources 有**两种**形式，实仓两种都在用：
#   A) 目标形式（6 处）：qt_add_resources(<target> <name> PREFIX p BASE b FILES f...)
#   B) 变量形式（1 处，cmake/MicroTeX.cmake:96）：qt_add_resources(<VAR> <file.qrc>)
# Qt5 的 qt5_add_resources 只支持 B。A 需要在这里手工生成 .qrc 再挂到目标上。
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_add_resources)
    function(qt_add_resources)
        list(LENGTH ARGV _snow_argc)
        if(_snow_argc LESS 2)
            message(FATAL_ERROR "qt_add_resources: expected at least 2 arguments, got ${_snow_argc}")
        endif()

        list(GET ARGV 1 _snow_second)
        if(_snow_second MATCHES "\\.qrc$")
            # ---- 形式 B：qt_add_resources(<VAR> <file.qrc>) ----
            list(GET ARGV 0 _snow_var)
            qt5_add_resources(${_snow_var} ${_snow_second})
            return()
        endif()

        # ---- 形式 A：目标形式 ----
        list(GET ARGV 0 _snow_target)
        if(NOT TARGET ${_snow_target})
            message(FATAL_ERROR
                "qt_add_resources: target '${_snow_target}' does not exist. "
                "The Qt6 target form requires the target to be created first.")
        endif()
        set(_snow_name ${_snow_second})

        set(_snow_rest ${ARGV})
        list(REMOVE_AT _snow_rest 0 1)
        cmake_parse_arguments(_snow_res "" "" "PREFIX;BASE;FILES;OPTIONS" ${_snow_rest})
        if(NOT _snow_res_FILES)
            message(FATAL_ERROR
                "qt_add_resources(${_snow_target} ${_snow_name}): no FILES given. "
                "Unsupported Qt6 argument combination for the Qt5 shim.")
        endif()

        # 复刻 Qt6 的别名计算：alias = 文件相对 BASE 的路径（BASE 缺省时取文件名）。
        set(_snow_qrc "<RCC>\n  <qresource prefix=\"${_snow_res_PREFIX}\">\n")
        foreach(_snow_file IN LISTS _snow_res_FILES)
            if(NOT IS_ABSOLUTE "${_snow_file}")
                set(_snow_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_snow_file}")
            else()
                set(_snow_abs "${_snow_file}")
            endif()
            file(RELATIVE_PATH _snow_alias "${_snow_res_BASE}" "${_snow_abs}")
            if(_snow_alias MATCHES "^\\.\\./" OR _snow_alias STREQUAL "")
                get_filename_component(_snow_alias "${_snow_abs}" NAME)
            endif()
            string(REPLACE "\\" "/" _snow_alias "${_snow_alias}")
            string(REPLACE "\\" "/" _snow_abs "${_snow_abs}")
            string(APPEND _snow_qrc
                "    <file alias=\"${_snow_alias}\">${_snow_abs}</file>\n")
        endforeach()
        string(APPEND _snow_qrc "  </qresource>\n</RCC>\n")

        set(_snow_qrc_file "${CMAKE_CURRENT_BINARY_DIR}/${_snow_target}_${_snow_name}.qrc")
        file(GENERATE OUTPUT "${_snow_qrc_file}" CONTENT "${_snow_qrc}")

        qt5_add_resources(_snow_qrc_sources "${_snow_qrc_file}")
        target_sources(${_snow_target} PRIVATE ${_snow_qrc_sources})
    endfunction()
endif()

if(NOT COMMAND qt_add_big_resources)
    function(qt_add_big_resources outfiles)
        qt5_add_big_resources(${outfiles} ${ARGN})
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 可执行目标：qt_add_executable -> add_executable + Qt6 的默认目标属性
#   * 实仓用到 MANUAL_FINALIZATION（cmake/SnowShotMini.cmake:113），add_executable 不认识，
#     必须先剥离。
#   * Qt6 在 Windows 上默认置 WIN32_EXECUTABLE 并链接入口点库；Qt5 的等价物是 qtmain，
#     以导入目标 Qt5::WinMain 暴露（已在本机套件确认存在）。
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_add_executable)
    function(qt_add_executable target)
        set(_snow_args ${ARGN})
        list(REMOVE_ITEM _snow_args MANUAL_FINALIZATION)
        add_executable(${target} ${_snow_args})
        if(WIN32)
            set_property(TARGET ${target} PROPERTY WIN32_EXECUTABLE TRUE)
            if(TARGET Qt5::WinMain)
                target_link_libraries(${target} PRIVATE Qt5::WinMain)
            else()
                message(STATUS
                    "Snow Qt shims: Qt5::WinMain not found; ${target} must provide its own "
                    "Windows entry point or the link step will fail.")
            endif()
        endif()
        if(APPLE)
            set_property(TARGET ${target} PROPERTY MACOSX_BUNDLE TRUE)
        endif()
        set_property(GLOBAL APPEND PROPERTY SNOW_QT5_EXECUTABLES ${target})
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 库目标：qt_add_library -> add_library
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_add_library)
    function(qt_add_library target)
        add_library(${target} ${ARGN})
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 项目级默认设置：qt_standard_project_setup -> 手工补齐等效默认值
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_standard_project_setup)
    function(qt_standard_project_setup)
        set(CMAKE_AUTOMOC ON PARENT_SCOPE)
        set(CMAKE_AUTOUIC ON PARENT_SCOPE)
        set(CMAKE_AUTORCC ON PARENT_SCOPE)
        if(NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 17)
            set(CMAKE_CXX_STANDARD 17 PARENT_SCOPE)
        endif()
        set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 翻译管线 1/2：qt_add_lrelease
#
# Qt6 有**两种**调用形态：
#   qt_add_lrelease(<target> TS_FILES ...)          # ≤ 6.6 的位置参数形态
#   qt_add_lrelease(TARGET <target> TS_FILES ...)   # 6.7+ 的关键字形态
# 本仓 cmake/SnowShotTranslationRelease.cmake 用的是**关键字在前**的形态，且只给
# LRELEASE_TARGET。因此 `function(qt_add_lrelease target)` 这种纯位置参数写法会把
# 字面量 "TS_FILES" 绑成 target，随后 cmake_parse_arguments 的关键字/值配对整体错位，
# 结果是 `_snow_lr_TS_FILES` 为空、一个 .qm 都不产出（只留一条 WARNING）。
# 这段代码直到 snow_shot 首次参与 Qt5 configure 才会被执行到，所以此前一直没暴露。
#
# Qt5: 用 lrelease 逐文件生成 .qm，并回填 QM_FILES_OUTPUT_VARIABLE。
# 唯一调用点是 cmake/SnowShotTranslationRelease.cmake。
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_add_lrelease)
    function(qt_add_lrelease)
        set(_snow_lr_arguments ${ARGN})
        set(_snow_lr_keywords
            TARGET TS_FILES LRELEASE_TARGET QM_FILES_OUTPUT_VARIABLE QM_OUTPUT_DIRECTORY
            OPTIONS QT_TRANSLATION_CATALOGS MERGE_QT_TRANSLATIONS)

        set(_snow_lr_positional_target "")
        if(_snow_lr_arguments)
            list(GET _snow_lr_arguments 0 _snow_lr_first)
            if(NOT _snow_lr_first IN_LIST _snow_lr_keywords)
                set(_snow_lr_positional_target "${_snow_lr_first}")
                list(REMOVE_AT _snow_lr_arguments 0)
            endif()
        endif()

        cmake_parse_arguments(_snow_lr
            "MERGE_QT_TRANSLATIONS"
            "TARGET;QM_FILES_OUTPUT_VARIABLE;LRELEASE_TARGET;QM_OUTPUT_DIRECTORY"
            "TS_FILES;OPTIONS;QT_TRANSLATION_CATALOGS"
            ${_snow_lr_arguments})

        set(_snow_lr_owner "${_snow_lr_TARGET}")
        if(NOT _snow_lr_owner)
            set(_snow_lr_owner "${_snow_lr_positional_target}")
        endif()

        _snow_qt_shim_bin_dir(_snow_lr_bin)
        find_program(SNOW_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5
            HINTS "${_snow_lr_bin}" NO_DEFAULT_PATH)
        if(NOT SNOW_LRELEASE_EXECUTABLE)
            find_program(SNOW_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5)
        endif()

        set(_snow_lr_target "${_snow_lr_LRELEASE_TARGET}")
        if(NOT _snow_lr_target)
            if(NOT _snow_lr_owner)
                message(FATAL_ERROR
                    "Snow Qt shims: qt_add_lrelease() needs a target name, given either "
                    "positionally, as TARGET, or as LRELEASE_TARGET.")
            endif()
            set(_snow_lr_target "${_snow_lr_owner}_lrelease")
        endif()
        if(NOT _snow_lr_owner)
            set(_snow_lr_owner "${_snow_lr_LRELEASE_TARGET}")
        endif()

        set(_snow_qm_dir "${_snow_lr_QM_OUTPUT_DIRECTORY}")
        if(NOT _snow_qm_dir)
            set(_snow_qm_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n")
        endif()
        file(MAKE_DIRECTORY "${_snow_qm_dir}")

        if(_snow_lr_QT_TRANSLATION_CATALOGS)
            message(STATUS
                "Snow Qt shims: qt_add_lrelease(${_snow_lr_owner}) MERGE_QT_TRANSLATION_CATALOGS "
                "(${_snow_lr_QT_TRANSLATION_CATALOGS}) is applied by the packaging step "
                "instead of lrelease under Qt5.")
        endif()

        set(_snow_qm_files)
        if(SNOW_LRELEASE_EXECUTABLE AND _snow_lr_TS_FILES)
            _snow_qt_translate_lrelease_options(
                _snow_lr_effective_options _snow_lr_enforce "${_snow_lr_OPTIONS}")
            set(_snow_lr_commands)
            foreach(_snow_ts IN LISTS _snow_lr_TS_FILES)
                get_filename_component(_snow_stem "${_snow_ts}" NAME_WE)
                set(_snow_qm "${_snow_qm_dir}/${_snow_stem}.qm")
                list(APPEND _snow_qm_files "${_snow_qm}")
                _snow_qt_lrelease_command(_snow_lr_one_command "${_snow_lr_enforce}"
                    "${SNOW_LRELEASE_EXECUTABLE}" "${_snow_ts}" "${_snow_qm}"
                    "${_snow_lr_effective_options}")
                list(APPEND _snow_lr_commands ${_snow_lr_one_command})
            endforeach()
            add_custom_target(${_snow_lr_target} ${_snow_lr_commands}
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                COMMENT "Snow Qt5 shim: lrelease ${_snow_lr_owner}"
                VERBATIM)
        else()
            message(WARNING
                "Snow Qt shims: lrelease or TS_FILES unavailable for ${_snow_lr_owner}; "
                "no .qm catalog will be produced for the Qt5 build.")
            add_custom_target(${_snow_lr_target}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Snow Qt5 shim: ${_snow_lr_owner} translation catalogs skipped "
                    "(lrelease or TS_FILES missing)")
        endif()

        if(_snow_lr_QM_FILES_OUTPUT_VARIABLE)
            set(${_snow_lr_QM_FILES_OUTPUT_VARIABLE} ${_snow_qm_files} PARENT_SCOPE)
        endif()
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 翻译管线 2/2：qt_add_translations
#
# Qt6: qt_add_translations(<target> TS_FILES ... [SOURCE_TARGETS ...] [SOURCES ...]
#                          [RESOURCE_PREFIX p] [LUPDATE_TARGET n] [LRELEASE_TARGET n]
#                          [LUPDATE_OPTIONS ...] [LRELEASE_OPTIONS ...])
#      会创建 lupdate/lrelease 目标，并把 .qm 以 RESOURCE_PREFIX 嵌入目标资源。
# Qt5: 手工建同名目标 + 生成 .qrc 走 qt5_add_resources 嵌入，语义与 Qt6 对齐。
# 唯一调用点是 ant_design_qt/CMakeLists.txt:153。
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_add_translations)
    function(qt_add_translations target)
        cmake_parse_arguments(_snow_tr
            ""
            "RESOURCE_PREFIX;SOURCE_LANGUAGE;LUPDATE_TARGET;LRELEASE_TARGET;QM_FILES_OUTPUT_VARIABLE"
            "TS_FILES;SOURCES;SOURCE_TARGETS;LUPDATE_OPTIONS;LRELEASE_OPTIONS;QT_TRANSLATION_CATALOGS"
            ${ARGN})

        _snow_qt_shim_bin_dir(_snow_tr_bin)
        find_program(SNOW_LUPDATE_EXECUTABLE NAMES lupdate lupdate-qt5
            HINTS "${_snow_tr_bin}" NO_DEFAULT_PATH)
        if(NOT SNOW_LUPDATE_EXECUTABLE)
            find_program(SNOW_LUPDATE_EXECUTABLE NAMES lupdate lupdate-qt5)
        endif()
        find_program(SNOW_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5
            HINTS "${_snow_tr_bin}" NO_DEFAULT_PATH)
        if(NOT SNOW_LRELEASE_EXECUTABLE)
            find_program(SNOW_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5)
        endif()

        # ---- lupdate 目标：刷新 .ts ----
        set(_snow_lu_target "${_snow_tr_LUPDATE_TARGET}")
        if(NOT _snow_lu_target)
            set(_snow_lu_target "${target}_lupdate")
        endif()
        if(SNOW_LUPDATE_EXECUTABLE AND _snow_tr_TS_FILES)
            add_custom_target(${_snow_lu_target}
                COMMAND "${SNOW_LUPDATE_EXECUTABLE}" ${_snow_tr_LUPDATE_OPTIONS}
                        ${_snow_tr_SOURCES} -ts ${_snow_tr_TS_FILES}
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                COMMENT "Snow Qt5 shim: lupdate ${target}"
                VERBATIM)
        else()
            message(STATUS
                "Snow Qt shims: lupdate or TS_FILES unavailable for ${target}; "
                "the ${_snow_lu_target} target is a no-op under Qt5.")
            add_custom_target(${_snow_lu_target}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Snow Qt5 shim: lupdate skipped for ${target}")
        endif()

        # ---- lrelease 目标：.ts -> .qm ----
        set(_snow_lr_target "${_snow_tr_LRELEASE_TARGET}")
        if(NOT _snow_lr_target)
            set(_snow_lr_target "${target}_lrelease")
        endif()
        set(_snow_qm_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n")
        file(MAKE_DIRECTORY "${_snow_qm_dir}")

        set(_snow_qm_files)
        if(SNOW_LRELEASE_EXECUTABLE AND _snow_tr_TS_FILES)
            _snow_qt_translate_lrelease_options(
                _snow_tr_effective_options _snow_tr_enforce "${_snow_tr_LRELEASE_OPTIONS}")
            foreach(_snow_ts IN LISTS _snow_tr_TS_FILES)
                get_filename_component(_snow_stem "${_snow_ts}" NAME_WE)
                set(_snow_qm "${_snow_qm_dir}/${_snow_stem}.qm")
                list(APPEND _snow_qm_files "${_snow_qm}")
                _snow_qt_lrelease_command(_snow_tr_lrelease_command "${_snow_tr_enforce}"
                    "${SNOW_LRELEASE_EXECUTABLE}" "${_snow_ts}" "${_snow_qm}"
                    "${_snow_tr_effective_options}")
                # 必须把 .qm 声明成 add_custom_command 的 **OUTPUT**，不能只让
                # add_custom_target 跑一条命令把它"顺带"写出来。
                #
                # 原因（实测于全新构建目录）：下面生成的 .qrc 会把 .qm 作为
                # <file> 列进资源清单，qt5_add_resources 于是把 .qm 写进
                # qrc_*_translations.cpp 这条边的**输入**里。Ninja 在加载构图阶段
                # 就要为每个输入找到"产出它的边"，找不到会直接报
                #   ninja: error: 'ant_design_qt/i18n/ant_design_qt_zh_CN.qm',
                #   needed by 'ant_design_qt/qrc_ant_design_qt_translations.cpp',
                #   missing and no known rule to make it
                # 而这个报错发生在**任何命令执行之前**，所以
                # add_dependencies(${target} ${_snow_lr_target}) 提供的先后顺序
                # 完全救不了它 —— 那条路径只有在 .qm 恰好已经躺在磁盘上时才走得通
                # （这正是它此前能"通过"的原因：上一轮构建残留了 .qm）。
                # 声明 OUTPUT 之后 Ninja 才会真正得到一条产出 .qm 的边，
                # 且每个 .qm 只依赖自己的 .ts，改一个 .ts 只重跑对应的一次 lrelease。
                add_custom_command(OUTPUT "${_snow_qm}"
                    ${_snow_tr_lrelease_command}
                    DEPENDS "${_snow_ts}"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    COMMENT "Snow Qt5 shim: lrelease ${_snow_stem}"
                    VERBATIM)
            endforeach()
            add_custom_target(${_snow_lr_target} DEPENDS ${_snow_qm_files})
        else()
            message(WARNING
                "Snow Qt shims: lrelease or TS_FILES unavailable for ${target}; "
                "no .qm catalog will be produced for the Qt5 build.")
            add_custom_target(${_snow_lr_target}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Snow Qt5 shim: lrelease skipped for ${target}")
        endif()

        if(_snow_tr_QM_FILES_OUTPUT_VARIABLE)
            set(${_snow_tr_QM_FILES_OUTPUT_VARIABLE} ${_snow_qm_files} PARENT_SCOPE)
        endif()

        # ---- 把 .qm 作为资源嵌入到目标（Qt6 会自动做，Qt5 需手工）----
        if(_snow_qm_files AND _snow_tr_RESOURCE_PREFIX AND TARGET ${target})
            set(_snow_qrc
                "<RCC>\n  <qresource prefix=\"${_snow_tr_RESOURCE_PREFIX}\">\n")
            foreach(_snow_qm IN LISTS _snow_qm_files)
                get_filename_component(_snow_qm_name "${_snow_qm}" NAME)
                string(REPLACE "\\" "/" _snow_qm "${_snow_qm}")
                string(APPEND _snow_qrc
                    "    <file alias=\"${_snow_qm_name}\">${_snow_qm}</file>\n")
            endforeach()
            string(APPEND _snow_qrc "  </qresource>\n</RCC>\n")

            set(_snow_qrc_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_translations.qrc")
            file(GENERATE OUTPUT "${_snow_qrc_file}" CONTENT "${_snow_qrc}")
            qt5_add_resources(_snow_translation_sources "${_snow_qrc_file}")
            target_sources(${target} PRIVATE ${_snow_translation_sources})
            # rcc 在构建期读取 .qm，必须先由 lrelease 生成。
            add_dependencies(${target} ${_snow_lr_target})
        elseif(NOT _snow_tr_RESOURCE_PREFIX)
            message(STATUS
                "Snow Qt shims: qt_add_translations(${target}) has no RESOURCE_PREFIX; "
                ".qm catalogs are produced but not embedded under Qt5.")
        endif()
    endfunction()
endif()

if(NOT COMMAND qt_add_translation)
    function(qt_add_translation outfiles)
        qt5_add_translation(${outfiles} ${ARGN})
    endfunction()
endif()

if(NOT COMMAND qt_add_lupdate)
    function(qt_add_lupdate target)
        message(STATUS
            "Snow Qt shims: qt_add_lupdate(${target}) has no Qt5 equivalent; "
            "refresh translations with the lupdate tool explicitly.")
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 静态插件导入：qt_import_plugins -> qt5_import_plugins
# qt5_import_plugins 自 Qt 5.14 起提供，且支持同样的 INCLUDE/EXCLUDE/
# INCLUDE_BY_TYPE/EXCLUDE_BY_TYPE 关键字（已核对 Qt5CoreMacros.cmake 实现）。
# 注意：插件名必须带命名空间，Qt5 下是 Qt5::<Plugin>，由 R1b 规则完成映射。
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_import_plugins)
    function(qt_import_plugins target)
        qt5_import_plugins(${target} ${ARGN})
    endfunction()
endif()

# ---------------------------------------------------------------------------
# moc 预处理：qt_wrap_cpp -> qt5_wrap_cpp
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_wrap_cpp)
    function(qt_wrap_cpp outfiles)
        qt5_wrap_cpp(${outfiles} ${ARGN})
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 部署脚本生成
#   Qt6: qt_generate_deploy_app_script -> 生成跨平台部署脚本
#   Qt5: Windows 由 windeployqt + 工程自带打包逻辑接管，这里登记备查
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_generate_deploy_app_script)
    function(qt_generate_deploy_app_script)
        set(_options NO_UNSUPPORTED_PLATFORM_ERROR NO_TRANSLATIONS)
        set(_one_value TARGET OUTPUT_SCRIPT DEPLOY_TOOL_OPTIONS DEPLOY_USER_OPTIONS PRE_INCLUDE_REGEXES PRE_EXCLUDE_REGEXES POST_INCLUDE_REGEXES POST_EXCLUDE_REGEXES)
        set(_multi_value)
        cmake_parse_arguments(_snow_deploy "${_options}" "${_one_value}" "${_multi_value}" ${ARGN})
        if(_snow_deploy_UNPARSED_ARGUMENTS)
            message(STATUS
                "Snow Qt shims: ignoring unsupported qt_generate_deploy_app_script arguments: "
                "${_snow_deploy_UNPARSED_ARGUMENTS}")
        endif()
        if(_snow_deploy_TARGET)
            set_property(GLOBAL APPEND PROPERTY SNOW_QT5_DEPLOY_TARGETS ${_snow_deploy_TARGET})
        endif()
        if(_snow_deploy_OUTPUT_SCRIPT)
            # 生成一个空脚本，保证调用方的 install(SCRIPT ...) 不会因文件缺失而失败。
            file(GENERATE OUTPUT "${_snow_deploy_OUTPUT_SCRIPT}"
                CONTENT "# Snow Qt5 shim: deployment is handled by the Snow packaging scripts.\n")
        endif()
        message(STATUS
            "Snow Qt shims: qt_generate_deploy_app_script is a no-op under Qt5; "
            "deployment for TARGET '${_snow_deploy_TARGET}' is handled by the Snow packaging scripts.")
    endfunction()
endif()

# ---------------------------------------------------------------------------
# finalize：Qt6 中必须在 install 前调用；Qt5 下无对应阶段
# ---------------------------------------------------------------------------
if(NOT COMMAND qt_finalize_executable)
    function(qt_finalize_executable target)
        # Qt5 无此阶段，保持空实现以兼容调用点。
    endfunction()
endif()

if(NOT COMMAND qt_finalize_project)
    function(qt_finalize_project)
        # 同上，空实现。
    endfunction()
endif()

# ---------------------------------------------------------------------------
# 便捷入口：解析"当前 Qt 命名空间"，供无法封装的调用点使用
# （get_target_property、if(TARGET ...)、$<TARGET_FILE:...> 等）。
# ---------------------------------------------------------------------------
function(snow_qt_namespace outvar)
    if(SNOW_QT_MAJOR STREQUAL "6")
        set(${outvar} "Qt6" PARENT_SCOPE)
    else()
        set(${outvar} "Qt5" PARENT_SCOPE)
    endif()
endfunction()
