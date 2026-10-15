include_guard(GLOBAL)

# =============================================================================
# Snow Apps —— Qt 工具链解析（支持 Qt 6 / Qt 5 双版本）
#
# 设计（见 docs-qt5-downgrade-plan.md §0、§2-L3）：
#   * SNOW_QT_MAJOR = 6（默认）：沿用原有审计契约 scripts/qt-toolchain.json，
#     行为与改造前完全一致，作为回归基线。
#   * SNOW_QT_MAJOR = 5：走 Qt 5.15 路线，并加载 cmake/SnowQtVersionShims.cmake
#     把 Qt6 风格的 qt_* 命令与 Qt::<模块> 目标补齐，使 882 处 CMake 调用点无需改写。
#
#   两个版本的差异被收敛到本文件与 shim 文件，业务 CMakeLists 不感知版本。
# =============================================================================

set(SNOW_QT_MAJOR "6" CACHE STRING
    "Qt major version to build against: 6 (audited Qt 6.12 kit) or 5 (Qt 5.15 kit)")
set_property(CACHE SNOW_QT_MAJOR PROPERTY STRINGS 6 5)
if(NOT SNOW_QT_MAJOR MATCHES "^[56]$")
    message(FATAL_ERROR "SNOW_QT_MAJOR must be 6 or 5, got '${SNOW_QT_MAJOR}'")
endif()
set_property(GLOBAL PROPERTY SNOW_QT_MAJOR "${SNOW_QT_MAJOR}")

# 关键：snow_find_qt / snow_qt_apply_compat_include 是 **macro**，宏内的
# CMAKE_CURRENT_LIST_DIR 指向「调用方」的目录而不是本文件目录，因此所有
# 相对本文件的路径都必须在这里（文件作用域，被 include 时求值一次）固化。
set(SNOW_QT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Qt 5 路线的最低版本要求。
# 依据：QStringDecoder / QLatin1StringView / qsizetype 之外的垫片已覆盖；
# 5.15 是 Qt 官方提供 versionless 目标（Qt::Core 等）与 versionless 命令的下限。
set(SNOW_QT5_MIN_VERSION "5.15.2")

if(SNOW_QT_MAJOR STREQUAL "6")
    set(SNOW_QT_NS "Qt6")
    set_property(GLOBAL PROPERTY SNOW_QT_NS "Qt6")

    # 审计契约：脚本与独立子工程共用同一份 Qt 工具链约束。
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../scripts/qt-toolchain.json")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../scripts/qt-toolchain.json" _snow_qt_contract)
    string(JSON _snow_qt_schema GET "${_snow_qt_contract}" schemaVersion)
    if(NOT _snow_qt_schema EQUAL 1)
        message(FATAL_ERROR "Unsupported Qt toolchain contract schema: ${_snow_qt_schema}")
    endif()
    string(JSON _snow_qt_version GET "${_snow_qt_contract}" qtVersion)
    string(JSON _snow_qt_deprecation GET "${_snow_qt_contract}" deprecationVersion)
    string(LENGTH "${_snow_qt_deprecation}" _snow_qt_deprecation_length)
    if(NOT _snow_qt_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" OR
       NOT _snow_qt_deprecation MATCHES "^0x[0-9a-fA-F]+$" OR
       NOT _snow_qt_deprecation_length EQUAL 8)
        message(FATAL_ERROR "The Qt toolchain contract contains an invalid version")
    endif()
    set_property(GLOBAL PROPERTY SNOW_QT_VERSION "${_snow_qt_version}")
    set_property(GLOBAL PROPERTY SNOW_QT_DEPRECATION_VERSION "${_snow_qt_deprecation}")
    unset(_snow_qt_contract)
    unset(_snow_qt_schema)
    unset(_snow_qt_version)
    unset(_snow_qt_deprecation)
    unset(_snow_qt_deprecation_length)
else()
    set(SNOW_QT_NS "Qt5")
    set_property(GLOBAL PROPERTY SNOW_QT_NS "Qt5")
    set_property(GLOBAL PROPERTY SNOW_QT_VERSION "${SNOW_QT5_MIN_VERSION}")
endif()

macro(snow_find_qt)
    # 兼容头（snow_shot/include/snow_shot/compat/qt_compat.h）在源码里是**显式**
    # #include 的：机械改写规则 R3 会把 Qt6 专有头（QEnterEvent 等）替换成它，
    # 因为 Qt6 构建下不会 force-include（只有 Qt5 分支注入 /FI）。
    # 因此这个搜索根必须对每个调用 snow_find_qt 的模块可见，否则
    # ant_design_qt / snow_draw_engine_qt / snow_image 这类并不依赖 snow_shot 的
    # 模块会直接报 "cannot open include file"。
    get_property(_snow_compat_root DIRECTORY PROPERTY SNOW_QT_COMPAT_ROOT_ADDED)
    if(NOT _snow_compat_root)
        get_filename_component(_snow_compat_inc
            "${SNOW_QT_CMAKE_DIR}/../snow_shot/include" ABSOLUTE)
        include_directories("${_snow_compat_inc}")
        set_property(DIRECTORY PROPERTY SNOW_QT_COMPAT_ROOT_ADDED TRUE)
        unset(_snow_compat_inc)
    endif()
    unset(_snow_compat_root)

    if(SNOW_QT_MAJOR STREQUAL "6")
        get_property(SNOW_QT_VERSION GLOBAL PROPERTY SNOW_QT_VERSION)
        find_package(Qt6 "${SNOW_QT_VERSION}" EXACT REQUIRED ${ARGN})
        if(NOT Qt6_VERSION VERSION_EQUAL SNOW_QT_VERSION)
            message(FATAL_ERROR
                "The resolved Qt ${Qt6_VERSION} does not match the audited Qt ${SNOW_QT_VERSION} kit")
        endif()
        # Directory definitions cover project-owned targets and Qt-generated resource
        # targets without changing the imported dependencies' compile interfaces.
        get_property(_snow_qt_policy_set DIRECTORY PROPERTY SNOW_QT_POLICY_SET)
        if(NOT _snow_qt_policy_set)
            get_property(_snow_qt_deprecation GLOBAL PROPERTY SNOW_QT_DEPRECATION_VERSION)
            add_compile_definitions("QT_DISABLE_DEPRECATED_UP_TO=${_snow_qt_deprecation}")
            set_property(DIRECTORY PROPERTY SNOW_QT_POLICY_SET TRUE)
            unset(_snow_qt_deprecation)
        endif()
        unset(_snow_qt_policy_set)
    else()
        # Qt5 不支持 EXACT 语义下的补丁级锁定（套件可能为 5.15.2 / 5.15.10 / 5.15.19），
        # 因此只锁"最低 5.15.2"。
        #
        # 说明：调用点会传入 CMake 的通用关键字 GLOBAL（CMake 3.24+ 起支持，
        # 用于把导入目标提升到全局作用域），它由 find_package 自身解析，
        # 不会被当成 Qt 组件名，因此这里原样透传是安全的（已实测）。
        find_package(Qt5 ${SNOW_QT5_MIN_VERSION} REQUIRED ${ARGN})
        set(SNOW_QT_VERSION "${Qt5_VERSION}")
        set_property(GLOBAL PROPERTY SNOW_QT_VERSION "${Qt5_VERSION}")

        # 关键一步：注入 Qt6 风格命令的兼容实现。
        # 注意：QT_DISABLE_DEPRECATED_UP_TO 是 Qt 6.5+ 专有宏，Qt5 下不能定义；
        # 相应地 Qt5 的 QT_DISABLE_DEPRECATED_BEFORE 也不启用——首轮目标是"能编译"，
        # 过早裁掉 Qt5 的 deprecated API 只会制造额外失败点。
        include("${SNOW_QT_CMAKE_DIR}/SnowQtVersionShims.cmake")

        message(STATUS "Snow Qt: Qt5 route active (${Qt5_VERSION}); Qt6-style commands shimmed.")

        # 目录级强制包含兼容头：注入一次，由后续 add_subdirectory 继承，
        # 从而避免在 112 个源文件里逐个补 #include。
        # 兼容头自身对"非 Qt 目标"与"仅 QtCore 目标"做了 __has_include 保护，
        # 因此对第三方（FetchContent）目标也是安全的。
        get_property(_snow_compat_injected DIRECTORY PROPERTY SNOW_QT5_COMPAT_INJECTED)
        if(NOT _snow_compat_injected)
            get_property(_snow_compat_header GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER)
            if(NOT _snow_compat_header)
                set(_snow_compat_header
                    "${SNOW_QT_CMAKE_DIR}/../snow_shot/include/snow_shot/compat/qt_compat.h")
                get_filename_component(_snow_compat_header "${_snow_compat_header}" ABSOLUTE)
                set_property(GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER "${_snow_compat_header}")
            endif()
            if(MSVC)
                add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/FI${_snow_compat_header}>")
            else()
                add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include;${_snow_compat_header}>")
            endif()
            set_property(DIRECTORY PROPERTY SNOW_QT5_COMPAT_INJECTED TRUE)
            message(STATUS "Snow Qt: Qt5 compat header force-included: ${_snow_compat_header}")
            unset(_snow_compat_header)
        endif()
        unset(_snow_compat_injected)
    endif()
endmacro()

# 手动把兼容头绑定到单个目标（仅当需要覆盖目录级注入时使用）。
macro(snow_qt_apply_compat_include target)
    if(SNOW_QT_MAJOR STREQUAL "5")
        get_property(_snow_compat_header GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER)
        if(NOT _snow_compat_header)
            set(_snow_compat_header
                "${SNOW_QT_CMAKE_DIR}/../snow_shot/include/snow_shot/compat/qt_compat.h")
            get_filename_component(_snow_compat_header "${_snow_compat_header}" ABSOLUTE)
            set_property(GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER "${_snow_compat_header}")
        endif()
        if(MSVC)
            target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:/FI${_snow_compat_header}>")
        else()
            target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:-include;${_snow_compat_header}>")
        endif()
        unset(_snow_compat_header)
    endif()
endmacro()

macro(snow_find_qt_private module)
    if(SNOW_QT_MAJOR STREQUAL "6")
        if(NOT Qt6_VERSION)
            message(FATAL_ERROR "Resolve Qt with snow_find_qt before requesting a private module")
        endif()
        # Private Qt modules have no cross-version ABI guarantee. Resolve them only
        # from the selected public kit, including for standalone application builds.
        find_package(Qt6${module}Private "${Qt6_VERSION}" EXACT REQUIRED CONFIG GLOBAL
            PATHS "${Qt6_DIR}/.." NO_DEFAULT_PATH)
        if(NOT Qt6${module}Private_VERSION VERSION_EQUAL Qt6_VERSION)
            message(FATAL_ERROR "The private Qt ${module} module does not match Qt ${Qt6_VERSION}")
        endif()
    else()
        # Qt5 的私有模块没有独立的 find_package 包装。
        # 5.15 通常直接导出 <Module>Private 目标；若未导出，则退回
        # Qt5<Module>_PRIVATE_INCLUDE_DIRS 变量（Qt5 官方文档承认的可用方式）。
        if(NOT TARGET Qt5::${module}Private)
            set_property(GLOBAL APPEND PROPERTY SNOW_QT5_PRIVATE_MODULES "${module}")
            message(STATUS
                "Snow Qt: Qt5 kit does not export Qt5::${module}Private; "
                "private headers fall back to Qt5${module}_PRIVATE_INCLUDE_DIRS "
                "(must be applied via target_include_directories at the call site).")
        endif()
    endif()
endmacro()

function(snow_qt_bin_directory output)
    if(SNOW_QT_MAJOR STREQUAL "6")
        # qtpaths is available independently of the optional qmake build feature.
        if(TARGET Qt6::qtpaths)
            get_target_property(_snow_qt_paths Qt6::qtpaths IMPORTED_LOCATION)
            if(NOT _snow_qt_paths)
                get_target_property(_snow_qt_configurations Qt6::qtpaths IMPORTED_CONFIGURATIONS)
                foreach(_snow_qt_configuration IN LISTS _snow_qt_configurations)
                    string(TOUPPER "${_snow_qt_configuration}" _snow_qt_configuration)
                    get_target_property(_snow_qt_paths Qt6::qtpaths
                        "IMPORTED_LOCATION_${_snow_qt_configuration}")
                    if(_snow_qt_paths)
                        break()
                    endif()
                endforeach()
            endif()
        endif()
        if(_snow_qt_paths)
            get_filename_component(_snow_qt_bin "${_snow_qt_paths}" DIRECTORY)
        elseif(QT6_INSTALL_PREFIX AND QT6_INSTALL_BINS)
            get_filename_component(_snow_qt_bin "${QT6_INSTALL_BINS}" ABSOLUTE
                BASE_DIR "${QT6_INSTALL_PREFIX}")
        else()
            message(FATAL_ERROR "The selected Qt kit does not expose its tools directory")
        endif()
    else()
        # Qt5：优先用 §SNOW_QT_BIN_DIR 覆盖，其次由 Qt5Core_DIR 反推 <prefix>/bin。
        if(SNOW_QT_BIN_DIR)
            set(_snow_qt_bin "${SNOW_QT_BIN_DIR}")
        elseif(Qt5Core_DIR)
            get_filename_component(_snow_qt_bin "${Qt5Core_DIR}/../../../bin" ABSOLUTE)
        elseif(QT5_INSTALL_PREFIX)
            set(_snow_qt_bin "${QT5_INSTALL_PREFIX}/bin")
        else()
            message(FATAL_ERROR
                "The selected Qt5 kit does not expose its tools directory; "
                "set SNOW_QT_BIN_DIR explicitly.")
        endif()
        unset(_snow_qt_paths)
    endif()
    set(${output} "${_snow_qt_bin}" PARENT_SCOPE)
endfunction()
