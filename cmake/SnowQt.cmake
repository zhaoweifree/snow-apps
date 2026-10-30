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

        # 兼容头的注入在这里统一处理（见下方），Qt5/Qt6 共用同一段逻辑。
    endif()

    # ---------------------------------------------------------------------
    # 目录级强制包含兼容头（Qt6 与 Qt5 **都**要注入）。
    #
    # 原先只有 Qt5 注入，假设 Qt6 走原生 API 因此不需要垫片。这个假设在机械
    # 改写铺开之后不再成立：R9/R18/R23 等规则把业务代码改成了 `SnowEnterEvent`
    # `snowAccentRole` `snowFontResolveMask` 这些垫片符号（仅 SnowEnterEvent 就有
    # 70 个文件），Qt6 不注入时它们全部未定义，Qt6 基线直接编译失败。
    #
    # 兼容头在 Qt6 下几乎全是"原生类型的薄别名"（SnowEnterEvent = QEnterEvent 等），
    # 注入没有行为开销；而它对"非 Qt 目标"和"仅 QtCore 目标"有 __has_include
    # 保护，对第三方 FetchContent 目标同样安全（Qt5 路线已长期验证）。
    #
    # 这样两边编译的是同一份源码，Qt5/Qt6 的差异只剩头文件里的 #if 分支，
    # 回归基线的对拍才成立。
    # ---------------------------------------------------------------------
    get_property(_snow_compat_injected DIRECTORY PROPERTY SNOW_QT_COMPAT_INJECTED)
    if(NOT _snow_compat_injected)
        get_property(_snow_compat_header GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER)
        if(NOT _snow_compat_header)
            set(_snow_compat_header
                "${SNOW_QT_CMAKE_DIR}/../snow_shot/include/snow_shot/compat/qt_compat.h")
            get_filename_component(_snow_compat_header "${_snow_compat_header}" ABSOLUTE)
            set_property(GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER "${_snow_compat_header}")
        endif()
        if(MSVC)
            # /utf-8 travels with the force-include: the header's comments are
            # UTF-8, and under the active code page (936 here) mispaired bytes
            # can swallow the following line. Without it the failure looks like
            # a stray "identifier not found" inside the header.
            add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
            add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/FI${_snow_compat_header}>")
        else()
            add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include;${_snow_compat_header}>")
        endif()
        set_property(DIRECTORY PROPERTY SNOW_QT_COMPAT_INJECTED TRUE)
        message(STATUS
            "Snow Qt: compat header force-included (Qt${SNOW_QT_MAJOR} route): ${_snow_compat_header}")
        unset(_snow_compat_header)
    endif()
    unset(_snow_compat_injected)
endmacro()

# 手动把兼容头绑定到单个目标（仅当需要覆盖目录级注入时使用）。
macro(snow_qt_apply_compat_include target)
    # 版本无关：Qt6 与 Qt5 编译的是同一份被改写过的源码，都需要垫片符号。
    get_property(_snow_compat_header GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER)
    if(NOT _snow_compat_header)
        set(_snow_compat_header
            "${SNOW_QT_CMAKE_DIR}/../snow_shot/include/snow_shot/compat/qt_compat.h")
        get_filename_component(_snow_compat_header "${_snow_compat_header}" ABSOLUTE)
        set_property(GLOBAL PROPERTY SNOW_QT_COMPAT_HEADER "${_snow_compat_header}")
    endif()
    if(MSVC)
        # /utf-8 belongs with the force-include: the shim header carries UTF-8
        # comments, and a target that does not already pass /utf-8 reads them as
        # the active code page (936 here). Mispaired bytes can then swallow the
        # following line, which surfaces as a bogus "identifier not found" a few
        # lines into the header rather than as an encoding problem.
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
            "$<$<COMPILE_LANGUAGE:CXX>:/FI${_snow_compat_header}>")
    else()
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-include;${_snow_compat_header}>")
    endif()
    unset(_snow_compat_header)
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
        # 带私有头的套件（官方安装包）里 Qt5GuiConfig.cmake 会建出 Qt5::GuiPrivate，
        # 但**没有任何目标去链接它**（各目标只链 Qt::Gui），于是
        # `#include <qpa/qplatformnativeinterface.h>` 会以
        # "fatal error C1083: 无法打开包括文件" 的形式失败 —— 看着像缺 Qt 组件，
        # 实际是缺一个 -I。这里直接把私有头目录加到**当前目录作用域**，
        # 覆盖 snow_shot/ 下所有目标（宏就是在 snow_shot/CMakeLists.txt 里调用的）。
        if(TARGET Qt5::${module}Private)
            get_target_property(_snow_priv_incs Qt5::${module}Private
                INTERFACE_INCLUDE_DIRECTORIES)
            if(_snow_priv_incs)
                include_directories(${_snow_priv_incs})
                message(STATUS
                    "Snow Qt: Qt5 private includes for ${module} applied to this directory: "
                    "${_snow_priv_incs}")
                unset(_snow_priv_incs)
            endif()
        else()
            # 发行版把私有头拆成单独的包：Debian/Ubuntu 的 qtbase5-dev 只有公开头，
            # `qtbase5-private-dev` 才提供 `<prefix>/QtGui/<ver>/{private,qpa}`。
            # 没有它 Qt5GuiConfig.cmake 不会建 Qt5::GuiPrivate（官方安装包带私有头才建），
            # 而仓库里 19 处 target_link_libraries 直接写 ${SNOW_QT_NS}::GuiPrivate ⇒
            # 配置期报 "Target links to Qt5::GuiPrivate"，只说目标名不说缺哪个包
            #（CI run 39600792545）。这里补一个同名 INTERFACE 目标，调用点一行不改；
            # 私有头真的没装就报出该装哪个包 —— 不静默降级，因为源码里
            # `<qpa/qwindowsysteminterface.h>`、`<private/qhighdpiscaling_p.h>` 是真需要的。
            include("${SNOW_QT_CMAKE_DIR}/SnowQt5InstallPaths.cmake")
            snow_qt5_private_include_dirs(_snow_priv_dirs ${module})
            if(NOT _snow_priv_dirs)
                message(FATAL_ERROR
                    "Qt5 ${module} private headers are not installed: neither "
                    "Qt5${module}_PRIVATE_INCLUDE_DIRS nor a "
                    "<QT_INSTALL_HEADERS>/${module}/<version> layout was found. "
                    "On Debian/Ubuntu install 'qtbase5-private-dev'; on other "
                    "distributions install the qtbase private headers package that "
                    "matches the kit in use.")
            endif()
            add_library(Qt5::${module}Private INTERFACE IMPORTED GLOBAL)
            set_target_properties(Qt5::${module}Private PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_snow_priv_dirs}"
                INTERFACE_LINK_LIBRARIES "Qt5::${module}")
            message(STATUS
                "Snow Qt: created Qt5::${module}Private from the kit's private headers: "
                "${_snow_priv_dirs}")
            unset(_snow_priv_dirs)
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
