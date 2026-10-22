include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/SnowWindowsArchitecture.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SnowRustLibclang.cmake")

# A Rust staticlib includes its dependencies and runtime. Consumers must select
# one archive for the final host rather than relying on link order to avoid
# extracting overlapping objects from two feature variants.
function(snow_add_rust_bundle_selector target default_bundle)
    if(NOT TARGET ${target})
        add_library(${target} INTERFACE)
        target_link_libraries(${target} INTERFACE
            "$<IF:$<BOOL:$<TARGET_PROPERTY:SNOW_RUST_BUNDLE_OVERRIDE>>,$<TARGET_PROPERTY:SNOW_RUST_BUNDLE_OVERRIDE>,${default_bundle}>")
    endif()
endfunction()

function(_snow_rust_target_reaches output root dependency)
    set(_pending ${root})
    set(_visited)
    while(_pending)
        list(POP_FRONT _pending _target)
        if(_target STREQUAL dependency)
            set(${output} TRUE PARENT_SCOPE)
            return()
        endif()
        if(_target IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited ${_target})
        get_target_property(_alias ${_target} ALIASED_TARGET)
        if(_alias)
            list(APPEND _pending ${_alias})
        endif()
        foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_items ${_target} ${_property})
            foreach(_item IN LISTS _items)
                if(TARGET "${_item}")
                    list(APPEND _pending "${_item}")
                elseif(_item MATCHES "\\$<")
                    # Unwrap LINK_ONLY/TARGET_OBJECTS while preserving aliases.
                    string(REGEX MATCHALL "[A-Za-z0-9_.:+-]+" _references "${_item}")
                    foreach(_reference IN LISTS _references)
                        string(REGEX REPLACE "(^|[^:]):([^:])" "\\1;\\2"
                            _names "${_reference}")
                        foreach(_name IN LISTS _names)
                            if(TARGET "${_name}")
                                list(APPEND _pending "${_name}")
                            endif()
                        endforeach()
                    endforeach()
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(${output} FALSE PARENT_SCOPE)
endfunction()

# Run after all executables have been declared. The override must be explicit
# on each final host: compatible interface properties are inferred too late to
# select that host's transitive link libraries.
function(snow_select_rust_bundle_consumers dependency bundle)
    set(_directories ${CMAKE_SOURCE_DIR})
    while(_directories)
        list(POP_FRONT _directories _directory)
        get_property(_children DIRECTORY "${_directory}" PROPERTY SUBDIRECTORIES)
        list(APPEND _directories ${_children})
        get_property(_targets DIRECTORY "${_directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            get_target_property(_type ${_target} TYPE)
            if(NOT _type STREQUAL "EXECUTABLE")
                continue()
            endif()
            _snow_rust_target_reaches(_uses_bundle ${_target} ${dependency})
            if(_uses_bundle)
                set_property(TARGET ${_target} PROPERTY SNOW_RUST_BUNDLE_OVERRIDE ${bundle})
            endif()
        endforeach()
    endwhile()
endfunction()

# Resolve the target OS before compiler family: AppleClang is not Linux, and
# GCC on Linux is not a MinGW cross compiler. Build universal macOS archives
# as two explicit Cargo targets and combine them with lipo at packaging time.
function(snow_detect_rust_target out_var)
    if(WIN32)
        snow_configure_windows_architecture()
        if(MSVC)
            if(SNOW_WINDOWS_ARCHITECTURE STREQUAL "arm64")
                set(_windows_target aarch64-pc-windows-msvc)
            else()
                set(_windows_target x86_64-pc-windows-msvc)
            endif()
        elseif(SNOW_WINDOWS_ARCHITECTURE STREQUAL "x64")
            set(_windows_target x86_64-pc-windows-gnu)
        else()
            message(FATAL_ERROR "Windows ARM64 requires the MSVC Rust target")
        endif()
    endif()
    if(DEFINED SNOW_RUST_TARGET AND NOT SNOW_RUST_TARGET STREQUAL "")
        if(WIN32 AND NOT SNOW_RUST_TARGET STREQUAL _windows_target)
            message(FATAL_ERROR
                "SNOW_RUST_TARGET=${SNOW_RUST_TARGET} disagrees with the ${SNOW_WINDOWS_ARCHITECTURE} C++ compiler; use ${_windows_target}")
        endif()
        set(${out_var} "${SNOW_RUST_TARGET}" PARENT_SCOPE)
        return()
    endif()
    if(APPLE)
        set(_arch "${CMAKE_OSX_ARCHITECTURES}")
        if(NOT _arch)
            set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(_arch MATCHES "^(arm64|aarch64)$")
            set(_target aarch64-apple-darwin)
        elseif(_arch STREQUAL "x86_64")
            set(_target x86_64-apple-darwin)
        else()
            message(FATAL_ERROR "Select one macOS architecture per Rust build: arm64 or x86_64")
        endif()
    elseif(WIN32)
        set(_target "${_windows_target}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(_target aarch64-unknown-linux-gnu)
        else()
            set(_target x86_64-unknown-linux-gnu)
        endif()
    else()
        message(FATAL_ERROR "Unsupported Rust host: ${CMAKE_SYSTEM_NAME}")
    endif()
    set(${out_var} "${_target}" PARENT_SCOPE)
endfunction()

function(snow_add_rust_static_libraries batch_name)
    set(options STRIP_MSVC_DIRECTIVES)
    set(oneValueArgs MANIFEST_DIR)
    set(multiValueArgs TARGETS PACKAGES OUTPUT_NAMES FEATURES)
    cmake_parse_arguments(SNOW_RUST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SNOW_RUST_MANIFEST_DIR)
        message(FATAL_ERROR "snow_add_rust_static_libraries requires MANIFEST_DIR")
    endif()
    foreach(_list_name IN ITEMS TARGETS PACKAGES OUTPUT_NAMES)
        list(LENGTH SNOW_RUST_${_list_name} _list_length)
        if(_list_length EQUAL 0)
            message(FATAL_ERROR
                "snow_add_rust_static_libraries requires a non-empty ${_list_name} list")
        endif()
        if(DEFINED _rust_library_count AND NOT _list_length EQUAL _rust_library_count)
            message(FATAL_ERROR
                "TARGETS, PACKAGES, and OUTPUT_NAMES must have the same length")
        endif()
        set(_rust_library_count ${_list_length})
    endforeach()

    find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
    if(NOT DEFINED SNOW_RUST_CARGO_TARGET_DIR OR SNOW_RUST_CARGO_TARGET_DIR STREQUAL "")
        set(SNOW_RUST_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo" CACHE PATH
            "Cargo target directory for the active CMake build tree.")
    endif()
    if(NOT DEFINED SNOW_RUST_RELEASE_PROFILE OR SNOW_RUST_RELEASE_PROFILE STREQUAL "")
        set(SNOW_RUST_RELEASE_PROFILE "release" CACHE STRING
            "Cargo profile used for non-Debug Rust builds.")
    endif()
    if(NOT DEFINED SNOW_VCPKG_ROOT OR SNOW_VCPKG_ROOT STREQUAL "")
        set(SNOW_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
    endif()
    if(NOT DEFINED SNOW_FFMPEG_ROOT OR SNOW_FFMPEG_ROOT STREQUAL "")
        if(DEFINED ENV{FFMPEG_DIR} AND NOT "$ENV{FFMPEG_DIR}" STREQUAL "")
            set(SNOW_FFMPEG_ROOT "$ENV{FFMPEG_DIR}")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Linux takes FFmpeg from the distribution (port document §5.1).
            # An empty prefix is the signal for "let pkg-config find it"; it
            # must not fall back to the vcpkg tree, which does not exist here.
            set(SNOW_FFMPEG_ROOT "")
        elseif(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
            set(SNOW_FFMPEG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        else()
            set(SNOW_FFMPEG_ROOT "${SNOW_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}")
        endif()
    endif()
    snow_detect_rust_target(SNOW_RUST_TARGET)

    # The cargo output is captured instead of discarded. `cargo metadata` is
    # the probe for "is this Rust target usable", and the reason it fails is
    # never the target itself: in CI run 39715098658 the target was fine and
    # the toolchain install had been rolled back, so the only useful line was
    # the one cargo printed and ERROR_QUIET threw away. The old message then
    # blamed the target and pointed at scripts/bootstrap.ps1 -- a Windows
    # script that does not exist on the Linux runners.
    execute_process(
        COMMAND "${CARGO_EXECUTABLE}" metadata --format-version 1
            --filter-platform "${SNOW_RUST_TARGET}" --no-deps
        WORKING_DIRECTORY "${SNOW_RUST_MANIFEST_DIR}"
        RESULT_VARIABLE _metadata_result
        OUTPUT_QUIET
        ERROR_VARIABLE _metadata_stderr
    )
    if(NOT _metadata_result EQUAL 0)
        string(STRIP "${_metadata_stderr}" _metadata_stderr)
        if(WIN32)
            set(_rust_remedy "Run scripts/bootstrap.ps1.")
        else()
            # string(CONCAT), not set() with two arguments: set() takes a
            # *list*, so the two values would be joined with a ';' and the
            # message would read "1.97.1 --profile minimal ;--target <triple>".
            # The remedy is one sentence and has to be copy-pasteable, so the
            # separator would be a defect, not cosmetics.
            string(CONCAT _rust_remedy
                "Install it with: rustup toolchain install 1.97.1 --profile minimal "
                "--target ${SNOW_RUST_TARGET}")
        endif()
        message(FATAL_ERROR
            "Rust target '${SNOW_RUST_TARGET}' is not installed or usable.\n"
            "cargo metadata --filter-platform ${SNOW_RUST_TARGET} exited with "
            "${_metadata_result}.\n"
            "${_rust_remedy}\n"
            "cargo said:\n${_metadata_stderr}")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(_profile "$<IF:$<CONFIG:Debug>,debug,${SNOW_RUST_RELEASE_PROFILE}>")
        set(_cargo_profile "$<IF:$<CONFIG:Debug>,dev,${SNOW_RUST_RELEASE_PROFILE}>")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_profile debug)
        set(_cargo_profile dev)
    else()
        set(_profile "${SNOW_RUST_RELEASE_PROFILE}")
        set(_cargo_profile "${SNOW_RUST_RELEASE_PROFILE}")
    endif()

    if(SNOW_RUST_TARGET MATCHES "msvc$")
        set(_lib_prefix "")
        set(_lib_suffix ".lib")
    else()
        set(_lib_prefix "lib")
        set(_lib_suffix ".a")
    endif()

    set(_static_libraries)
    set(_debug_libraries)
    set(_release_libraries)
    set(_cargo_package_args)
    math(EXPR _rust_library_last "${_rust_library_count} - 1")
    foreach(_index RANGE ${_rust_library_last})
        list(GET SNOW_RUST_PACKAGES ${_index} _package)
        list(GET SNOW_RUST_OUTPUT_NAMES ${_index} _output_name)
        string(REPLACE "-" "_" _output_stem "${_output_name}")
        list(APPEND _cargo_package_args --package "${_package}")
        list(APPEND _static_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/${_profile}/${_lib_prefix}${_output_stem}${_lib_suffix}")
        list(APPEND _debug_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/debug/${_lib_prefix}${_output_stem}${_lib_suffix}")
        list(APPEND _release_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/${SNOW_RUST_RELEASE_PROFILE}/${_lib_prefix}${_output_stem}${_lib_suffix}")
    endforeach()

    snow_resolve_rust_libclang_directory(_libclang_dir)

    set(_snow_rust_static_crt FALSE)
    if(SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC OR
       (MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" AND
        NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
        set(_snow_rust_static_crt TRUE)
    endif()

    set(_vcpkg_dynamic 1)
    if(SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC)
        set(_vcpkg_dynamic 0)
    endif()
    set(_cargo_environment
        "VCPKG_ROOT=${SNOW_VCPKG_ROOT}"
        "VCPKGRS_TRIPLET=${VCPKG_TARGET_TRIPLET}"
        "VCPKGRS_DYNAMIC=${_vcpkg_dynamic}"
        "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
    )
    # FFMPEG_DIR must be *absent*, not empty, when there is no prebuilt FFmpeg
    # prefix. ffmpeg-sys-next's build script branches on `env::var("FFMPEG_DIR")`
    # succeeding, so an empty value still selects the prebuilt branch, where it
    # searches <FFMPEG_DIR>/include alone and never falls back to pkg-config.
    if(NOT SNOW_FFMPEG_ROOT STREQUAL "")
        list(APPEND _cargo_environment "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}")
    endif()
    snow_windows_rust_environment(_windows_cargo_environment "${SNOW_RUST_TARGET}")
    list(APPEND _cargo_environment ${_windows_cargo_environment})
    if(APPLE)
        set(_snow_rust_deployment_target "15.0")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(_snow_rust_deployment_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        list(APPEND _cargo_environment
            "MACOSX_DEPLOYMENT_TARGET=${_snow_rust_deployment_target}"
            "CARGO_PROFILE_RELEASE_DEBUG=2" "CARGO_PROFILE_RELEASE_FAST_DEBUG=2")
    endif()
    if(_libclang_dir)
        list(APPEND _cargo_environment "LIBCLANG_PATH=${_libclang_dir}")
    endif()

    if(MSVC AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug" OR
                 SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC))
        if(_snow_rust_static_crt)
            set(_rust_debug_runtime "/MTd /D_DEBUG")
            set(_rust_release_runtime "/MT")
        else()
            set(_rust_debug_runtime "/MDd /D_DEBUG")
            set(_rust_release_runtime "/MD")
        endif()
        if(CMAKE_CONFIGURATION_TYPES)
            set(_rust_cxxflags
                "$<IF:$<CONFIG:Debug>,$ENV{CXXFLAGS} ${_rust_debug_runtime},$ENV{CXXFLAGS} ${_rust_release_runtime}>")
            set(_rust_cflags
                "$<IF:$<CONFIG:Debug>,$ENV{CFLAGS} ${_rust_debug_runtime},$ENV{CFLAGS} ${_rust_release_runtime}>")
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_rust_cxxflags "$ENV{CXXFLAGS} ${_rust_debug_runtime}")
            set(_rust_cflags "$ENV{CFLAGS} ${_rust_debug_runtime}")
        else()
            set(_rust_cxxflags "$ENV{CXXFLAGS} ${_rust_release_runtime}")
            set(_rust_cflags "$ENV{CFLAGS} ${_rust_release_runtime}")
        endif()
        list(INSERT _cargo_environment 0
            "CXXFLAGS=${_rust_cxxflags}"
            "CFLAGS=${_rust_cflags}"
        )
        if(_snow_rust_static_crt)
            list(INSERT _cargo_environment 0
                "RUSTFLAGS=$ENV{RUSTFLAGS} -Dwarnings -C target-feature=+crt-static")
        endif()
    endif()

    set(_cargo_feature_args)
    if(SNOW_RUST_FEATURES)
        list(JOIN SNOW_RUST_FEATURES "," _cargo_features)
        list(APPEND _cargo_feature_args --features "${_cargo_features}")
    endif()
    set(_cargo_commands
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_cargo_environment}
            "${CARGO_EXECUTABLE}" build --locked
            ${_cargo_package_args}
            ${_cargo_feature_args}
            --target "${SNOW_RUST_TARGET}"
            --profile "${_cargo_profile}"
    )
    if(SNOW_RUST_STRIP_MSVC_DIRECTIVES AND SNOW_RUST_TARGET MATCHES "gnu$")
        find_program(RUST_ARCHIVE_OBJCOPY NAMES llvm-objcopy objcopy REQUIRED)
        foreach(_static_library IN LISTS _static_libraries)
            list(APPEND _cargo_commands
                COMMAND "${RUST_ARCHIVE_OBJCOPY}" --remove-section=.drectve "${_static_library}")
        endforeach()
    endif()

    add_custom_target("${batch_name}_build"
        ${_cargo_commands}
        BYPRODUCTS ${_static_libraries}
        WORKING_DIRECTORY "${SNOW_RUST_MANIFEST_DIR}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    foreach(_index RANGE ${_rust_library_last})
        list(GET SNOW_RUST_TARGETS ${_index} _target_name)
        list(GET _static_libraries ${_index} _static_library)
        list(GET _debug_libraries ${_index} _debug_library)
        list(GET _release_libraries ${_index} _release_library)
        add_library("${_target_name}" STATIC IMPORTED GLOBAL)
        if(CMAKE_CONFIGURATION_TYPES)
            set_target_properties("${_target_name}" PROPERTIES
                IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
                IMPORTED_LOCATION_DEBUG "${_debug_library}"
                IMPORTED_LOCATION_RELEASE "${_release_library}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${_release_library}"
                IMPORTED_LOCATION_MINSIZEREL "${_release_library}"
            )
        else()
            set_target_properties("${_target_name}" PROPERTIES
                IMPORTED_LOCATION "${_static_library}")
        endif()
        add_dependencies("${_target_name}" "${batch_name}_build")
    endforeach()
endfunction()

function(snow_add_rust_static_library target_name)
    set(options STRIP_MSVC_DIRECTIVES)
    set(oneValueArgs PACKAGE MANIFEST_DIR OUTPUT_NAME)
    cmake_parse_arguments(SNOW_RUST "${options}" "${oneValueArgs}" "FEATURES" ${ARGN})

    foreach(_required IN ITEMS PACKAGE MANIFEST_DIR)
        if(NOT SNOW_RUST_${_required})
            message(FATAL_ERROR "snow_add_rust_static_library requires ${_required}")
        endif()
    endforeach()
    if(NOT SNOW_RUST_OUTPUT_NAME)
        set(SNOW_RUST_OUTPUT_NAME "${SNOW_RUST_PACKAGE}")
    endif()
    set(_strip_option)
    if(SNOW_RUST_STRIP_MSVC_DIRECTIVES)
        set(_strip_option STRIP_MSVC_DIRECTIVES)
    endif()
    snow_add_rust_static_libraries("${target_name}_rust"
        MANIFEST_DIR "${SNOW_RUST_MANIFEST_DIR}"
        TARGETS "${target_name}"
        PACKAGES "${SNOW_RUST_PACKAGE}"
        OUTPUT_NAMES "${SNOW_RUST_OUTPUT_NAME}"
        FEATURES ${SNOW_RUST_FEATURES}
        ${_strip_option}
    )
endfunction()

function(snow_add_rust_executable target_name)
    set(options PERFORMANCE_PROFILE NO_DEFAULT_FEATURES REPRODUCIBLE SIZE_OPTIMIZED)
    set(oneValueArgs PACKAGE MANIFEST_DIR OUTPUT_NAME PRODUCTION_PROFILE)
    set(multiValueArgs FEATURES ENVIRONMENT)
    cmake_parse_arguments(SNOW_RUST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    foreach(_required IN ITEMS PACKAGE MANIFEST_DIR)
        if(NOT SNOW_RUST_${_required})
            message(FATAL_ERROR "snow_add_rust_executable requires ${_required}")
        endif()
    endforeach()
    if(NOT SNOW_RUST_OUTPUT_NAME)
        set(SNOW_RUST_OUTPUT_NAME "${SNOW_RUST_PACKAGE}")
    endif()
    find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
    if(NOT DEFINED SNOW_RUST_CARGO_TARGET_DIR OR SNOW_RUST_CARGO_TARGET_DIR STREQUAL "")
        set(SNOW_RUST_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo" CACHE PATH
            "Cargo target directory for the active CMake build tree.")
    endif()
    if(NOT DEFINED SNOW_RUST_RELEASE_PROFILE OR SNOW_RUST_RELEASE_PROFILE STREQUAL "")
        set(SNOW_RUST_RELEASE_PROFILE "release" CACHE STRING
            "Cargo profile used for non-Debug Rust builds.")
    endif()
    set(_selected_release_profile "${SNOW_RUST_RELEASE_PROFILE}")
    # The fast preset is explicitly intended to avoid expensive full LTO, even for
    # executables which use a dedicated production profile in release packages.
    if(SNOW_RUST_PRODUCTION_PROFILE AND NOT SNOW_RUST_RELEASE_PROFILE STREQUAL "release-fast")
        set(_selected_release_profile "${SNOW_RUST_PRODUCTION_PROFILE}")
    endif()
    snow_detect_rust_target(_rust_target)
    if(SNOW_RUST_PERFORMANCE_PROFILE)
        # The executable is performance-critical and independent of the CMake
        # configuration, so always build it with the release profile.
        set(_profile "${_selected_release_profile}")
        set(_cargo_profile "${_selected_release_profile}")
    elseif(CMAKE_CONFIGURATION_TYPES)
        set(_profile "$<IF:$<CONFIG:Debug>,debug,${_selected_release_profile}>")
        set(_cargo_profile "$<IF:$<CONFIG:Debug>,dev,${_selected_release_profile}>")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_profile debug)
        set(_cargo_profile dev)
    else()
        set(_profile "${_selected_release_profile}")
        set(_cargo_profile "${_selected_release_profile}")
    endif()
    if(WIN32)
        set(_binary_suffix ".exe")
    else()
        set(_binary_suffix)
    endif()
    set(_binary_path
        "${SNOW_RUST_CARGO_TARGET_DIR}/${_rust_target}/${_profile}/${SNOW_RUST_OUTPUT_NAME}${_binary_suffix}")
    set(_debug_binary_path
        "${SNOW_RUST_CARGO_TARGET_DIR}/${_rust_target}/debug/${SNOW_RUST_OUTPUT_NAME}${_binary_suffix}")
    set(_release_binary_path
        "${SNOW_RUST_CARGO_TARGET_DIR}/${_rust_target}/${_selected_release_profile}/${SNOW_RUST_OUTPUT_NAME}${_binary_suffix}")
    set(_cargo_environment
        "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}")
    snow_windows_rust_environment(_windows_cargo_environment "${_rust_target}")
    list(APPEND _cargo_environment ${_windows_cargo_environment})
    if(WIN32 AND (SNOW_LIBCLANG_BIN_DIR OR DEFINED ENV{LIBCLANG_PATH}))
        snow_resolve_rust_libclang_directory(_libclang_dir)
        list(APPEND _cargo_environment "LIBCLANG_PATH=${_libclang_dir}")
    endif()
    if(DEFINED VCPKG_ROOT)
        list(APPEND _cargo_environment "VCPKG_ROOT=${VCPKG_ROOT}")
    elseif(DEFINED ENV{VCPKG_ROOT})
        list(APPEND _cargo_environment "VCPKG_ROOT=$ENV{VCPKG_ROOT}")
    endif()
    if(DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _cargo_environment "VCPKGRS_TRIPLET=${VCPKG_TARGET_TRIPLET}")
    endif()
    if(SNOW_SHOT_RELEASE_STATIC OR SNOW_APPS_RELEASE_STATIC)
        list(APPEND _cargo_environment "VCPKGRS_DYNAMIC=0")
    else()
        list(APPEND _cargo_environment "VCPKGRS_DYNAMIC=1")
    endif()
    if(APPLE)
        set(_snow_rust_deployment_target "15.0")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(_snow_rust_deployment_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        list(APPEND _cargo_environment
            "MACOSX_DEPLOYMENT_TARGET=${_snow_rust_deployment_target}")
    endif()
    if(SNOW_RUST_ENVIRONMENT)
        list(APPEND _cargo_environment ${SNOW_RUST_ENVIRONMENT})
    endif()
    if(MSVC)
        set(_rust_release_link_flags)
        if(SNOW_RUST_REPRODUCIBLE)
            set(_rust_release_link_flags " -C link-arg=/Brepro")
        endif()
        if(SNOW_RUST_SIZE_OPTIMIZED)
            string(APPEND _rust_release_link_flags
                " -C link-arg=/OPT:REF -C link-arg=/OPT:ICF -C link-arg=/INCREMENTAL:NO -C link-arg=/DEBUG:FULL")
        endif()
        set(_rust_link_flags "${_rust_release_link_flags}")
        # Size-tuned link flags belong to the production profile. A multi-config
        # Debug build must retain Cargo's ordinary development-link behavior.
        if(SNOW_RUST_SIZE_OPTIMIZED AND NOT SNOW_RUST_PERFORMANCE_PROFILE)
            if(CMAKE_CONFIGURATION_TYPES)
                set(_rust_link_flags "$<IF:$<CONFIG:Debug>,,${_rust_release_link_flags}>")
            elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
                set(_rust_link_flags)
            endif()
        endif()
        set(_snow_rust_static_crt FALSE)
        if(SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC OR
           (CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" AND
            NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
            set(_snow_rust_static_crt TRUE)
        endif()
        # Select the CRT flavor from the CMake configuration. In a multi-config
        # generator, _cargo_profile is itself a generator expression and cannot
        # be compared with STREQUAL at configure time. Performance executables
        # deliberately retain their release CRT even in a Debug configuration.
        if(_snow_rust_static_crt)
            set(_rust_debug_runtime "/MTd /D_DEBUG")
            set(_rust_release_runtime "/MT")
        else()
            set(_rust_debug_runtime "/MDd /D_DEBUG")
            set(_rust_release_runtime "/MD")
        endif()
        if(SNOW_RUST_PERFORMANCE_PROFILE)
            set(_rust_c_runtime "${_rust_release_runtime}")
        elseif(CMAKE_CONFIGURATION_TYPES)
            set(_rust_c_runtime
                "$<IF:$<CONFIG:Debug>,${_rust_debug_runtime},${_rust_release_runtime}>")
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_rust_c_runtime "${_rust_debug_runtime}")
        else()
            set(_rust_c_runtime "${_rust_release_runtime}")
        endif()
        list(APPEND _cargo_environment
            "CXXFLAGS=$ENV{CXXFLAGS} ${_rust_c_runtime}"
            "CFLAGS=$ENV{CFLAGS} ${_rust_c_runtime}"
        )
        if(_snow_rust_static_crt)
            list(INSERT _cargo_environment 0
                "RUSTFLAGS=$ENV{RUSTFLAGS} -Dwarnings -C target-feature=+crt-static${_rust_link_flags}")
        elseif(SNOW_RUST_REPRODUCIBLE OR SNOW_RUST_SIZE_OPTIMIZED)
            list(INSERT _cargo_environment 0
                "RUSTFLAGS=$ENV{RUSTFLAGS} -Dwarnings${_rust_link_flags}")
        endif()
    elseif(UNIX AND NOT APPLE)
        # The cargo-built executables are installed with
        # `install(PROGRAMS "$<TARGET_FILE:...>")`, which copies the file
        # verbatim. `INSTALL_RPATH` is a property of *targets* installed through
        # `install(TARGETS)` -- CMake edits the RUNPATH of those on the way in --
        # and a cargo executable is an `IMPORTED` target that never takes that
        # path. So the runpath has to be written by the linker, once, here.
        #
        # Without it the staged helper carries no `DT_RUNPATH` at all: the loader
        # searches only the system directories, the private libraries the package
        # ships in `${CMAKE_INSTALL_LIBDIR}/snow-shot` are unreachable, and the
        # package installs cleanly while its OCR helper cannot start. It is also
        # invisible to `dpkg-shlibdeps`: `CPackDeb.cmake` derives its `-l<dir>`
        # arguments from the RUNPATH, so a binary without one contributes
        # nothing and the private copies have to be found through
        # `SHLIBDEPS_PRIVATE_DIRS` alone.
        #
        # The directory name mirrors `snow_shot_linux_private_lib_dir()`. That
        # function lives in a module included after the subdirectories, so it
        # cannot be called from here; the packaging gate compares the two.
        set(_snow_rust_libdir "${CMAKE_INSTALL_LIBDIR}")
        if(NOT _snow_rust_libdir)
            set(_snow_rust_libdir "lib")
        endif()
        # `$ORIGIN` is literal, not a CMake variable: the `$` is put together
        # here so that no `$` + `{` sequence can be read as an expansion.
        set(_snow_rust_dollar "$")
        set(_snow_rust_rpath
            "${_snow_rust_dollar}ORIGIN/../${_snow_rust_libdir}/snow-shot")
        list(INSERT _cargo_environment 0
            "RUSTFLAGS=$ENV{RUSTFLAGS} -C link-arg=-Wl,-rpath,${_snow_rust_rpath}")
    endif()
    set(_cargo_feature_args)
    if(SNOW_RUST_NO_DEFAULT_FEATURES)
        list(APPEND _cargo_feature_args --no-default-features)
    endif()
    if(SNOW_RUST_FEATURES)
        list(JOIN SNOW_RUST_FEATURES "," _cargo_features)
        list(APPEND _cargo_feature_args --features "${_cargo_features}")
    endif()
    add_custom_target("${target_name}_build"
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_cargo_environment}
            "${CARGO_EXECUTABLE}" build --locked
            --package "${SNOW_RUST_PACKAGE}"
            --target "${_rust_target}"
            --profile "${_cargo_profile}"
            ${_cargo_feature_args}
        BYPRODUCTS "${_binary_path}"
        WORKING_DIRECTORY "${SNOW_RUST_MANIFEST_DIR}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_executable("${target_name}" IMPORTED GLOBAL)
    if(SNOW_RUST_PERFORMANCE_PROFILE)
        set_target_properties("${target_name}" PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
            IMPORTED_LOCATION "${_release_binary_path}"
            IMPORTED_LOCATION_DEBUG "${_release_binary_path}"
            IMPORTED_LOCATION_RELEASE "${_release_binary_path}"
            IMPORTED_LOCATION_RELWITHDEBINFO "${_release_binary_path}"
            IMPORTED_LOCATION_MINSIZEREL "${_release_binary_path}"
        )
    elseif(CMAKE_CONFIGURATION_TYPES)
        set_target_properties("${target_name}" PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
            IMPORTED_LOCATION_DEBUG "${_debug_binary_path}"
            IMPORTED_LOCATION_RELEASE "${_release_binary_path}"
            IMPORTED_LOCATION_RELWITHDEBINFO "${_release_binary_path}"
            IMPORTED_LOCATION_MINSIZEREL "${_release_binary_path}"
        )
    else()
        set_target_properties("${target_name}" PROPERTIES IMPORTED_LOCATION "${_binary_path}")
    endif()
    add_dependencies("${target_name}" "${target_name}_build")
    set_property(TARGET "${target_name}" PROPERTY
        SNOW_RUST_PROFILE_DIRECTORY
        "${SNOW_RUST_CARGO_TARGET_DIR}/${_rust_target}/${_selected_release_profile}")
endfunction()
