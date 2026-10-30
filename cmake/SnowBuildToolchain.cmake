# Triplets and toolchains can be loaded in different function scopes. Reload the
# minimums in each scope along with the functions that consume them.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../scripts/build-toolchain.json" _snow_build_toolchain)
string(JSON SNOW_MSVC_TOOLSET_MINIMUM GET "${_snow_build_toolchain}" msvcToolsetMinimum)
string(JSON SNOW_MSVC_COMPILER_MINIMUM GET "${_snow_build_toolchain}" msvcCompilerMinimum)
string(JSON SNOW_RUST_MINIMUM GET "${_snow_build_toolchain}" rustMinimum)
unset(_snow_build_toolchain)

function(snow_require_msvc_version)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS SNOW_MSVC_COMPILER_MINIMUM)
        message(FATAL_ERROR
            "MSVC ${SNOW_MSVC_TOOLSET_MINIMUM} or newer is required; found compiler ${CMAKE_CXX_COMPILER_VERSION}.")
    endif()
endfunction()

function(snow_require_rust_version)
    set(_working_directory "${CMAKE_CURRENT_SOURCE_DIR}")
    if(ARGC GREATER 0)
        set(_working_directory "${ARGV0}")
    endif()
    find_program(RUSTC_EXECUTABLE NAMES rustc REQUIRED)
    execute_process(COMMAND "${RUSTC_EXECUTABLE}" --version
        WORKING_DIRECTORY "${_working_directory}"
        OUTPUT_VARIABLE _version OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _result ERROR_QUIET)
    if(NOT _result EQUAL 0 OR NOT _version MATCHES "^rustc ([0-9]+\\.[0-9]+\\.[0-9]+)")
        message(FATAL_ERROR "Unable to determine the active Rust version. Run the bootstrap script.")
    endif()
    if(CMAKE_MATCH_1 VERSION_LESS SNOW_RUST_MINIMUM)
        message(FATAL_ERROR "Rust ${SNOW_RUST_MINIMUM} or newer is required; found ${_version}.")
    endif()
endfunction()

# Share selection between vcpkg's vcvars environment and its chainloaded CMake
# toolchain so ports cannot silently compile with different MSVC versions.
function(snow_resolve_msvc_tools_root output)
    set(_root "$ENV{VCToolsInstallDir}")
    if(_root STREQUAL "")
        set(_vs "$ENV{VSINSTALLDIR}")
        if(_vs STREQUAL "")
            find_program(_vswhere vswhere.exe PATHS
                "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer"
                "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer")
            if(_vswhere)
                set(_component Microsoft.VisualStudio.Component.VC.Tools.x86.x64)
                if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
                    set(_component Microsoft.VisualStudio.Component.VC.Tools.ARM64)
                endif()
                execute_process(COMMAND "${_vswhere}" -latest -products *
                    -requires "${_component}" -property installationPath
                    OUTPUT_VARIABLE _vs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            endif()
        endif()
        file(GLOB _candidates LIST_DIRECTORIES TRUE "${_vs}/VC/Tools/MSVC/*")
        foreach(_candidate IN LISTS _candidates)
            get_filename_component(_version "${_candidate}" NAME)
            if(IS_DIRECTORY "${_candidate}" AND _version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" AND
               NOT _version VERSION_LESS SNOW_MSVC_TOOLSET_MINIMUM)
                if(_root STREQUAL "" OR _version VERSION_GREATER _selected_version)
                    set(_root "${_candidate}")
                    set(_selected_version "${_version}")
                endif()
            endif()
        endforeach()
    endif()
    string(REGEX REPLACE "[/\\]+$" "" _root "${_root}")
    file(TO_CMAKE_PATH "${_root}" _root)
    get_filename_component(_version "${_root}" NAME)
    if(NOT IS_DIRECTORY "${_root}" OR NOT _version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" OR
       _version VERSION_LESS SNOW_MSVC_TOOLSET_MINIMUM)
        message(FATAL_ERROR
            "MSVC ${SNOW_MSVC_TOOLSET_MINIMUM} or newer is required; selected '${_root}'. Install the required Build Tools or set VSINSTALLDIR/VCToolsInstallDir.")
    endif()
    set(${output} "${_root}" PARENT_SCOPE)
endfunction()
