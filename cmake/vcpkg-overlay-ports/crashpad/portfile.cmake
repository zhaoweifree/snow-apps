# The pinned upstream port interpolates MSVC flags into GN string literals
# without escaping SDK include paths. Keep its source/patch set and escape only
# the variables entering those literals (the workspace chainload adds SDK paths).
set(_snow_upstream "${VCPKG_ROOT_DIR}/ports/crashpad")
# Snow's release Windows executables and OCR runtime use the static CRT, so this
# port forces it regardless of what the triplet says. The Qt5 verification route
# links a *shared* Qt 5.15.2 kit, which means every static library on its link
# line must be built for the dynamic CRT (/MDd in the Debug preset) -- a /MT
# crashpad fails there with LNK2038 `RuntimeLibrary: MT_StaticRelease`.
#
# SNOW_CRASHPAD_DYNAMIC_CRT is therefore set by the mirror overlay port
# (.workbuddy/qt5-port/vcpkg-mirror/overlay-ports/crashpad), which only joins the
# overlay chain while that route is being validated; release builds never see it
# and keep the static CRT. Same trade-off as opencv4: one vcpkg_installed tree
# can only hold one CRT flavor of a given port.
if(VCPKG_TARGET_IS_WINDOWS AND NOT SNOW_CRASHPAD_DYNAMIC_CRT)
    set(VCPKG_CRT_LINKAGE static)
endif()
# Visible to the escape block below (include() shares the caller's scope).
set(_snow_crashpad_static_crt TRUE)
if(VCPKG_TARGET_IS_WINDOWS AND SNOW_CRASHPAD_DYNAMIC_CRT)
    set(_snow_crashpad_static_crt FALSE)
endif()
file(READ "${_snow_upstream}/portfile.cmake" _snow_port)
string(REPLACE "z;zlib;zlibd" "z;zlib;zlibd;zlibstatic;zlibstaticd" _snow_port "${_snow_port}")
foreach(_snow_patch fix-linux.patch fix-lib-name-conflict.patch crashpad-memset-errors-5758170.diff
        fix-std-20.patch ndk-toolchain.diff fix-lib-name-conflict-1.patch)
    string(REPLACE "        ${_snow_patch}" "        \"${_snow_upstream}/${_snow_patch}\""
        _snow_port "${_snow_port}")
endforeach()
if(VCPKG_TARGET_IS_OSX)
    # Honor the triplet's minimum OS and tolerate deprecated SDK APIs in pinned
    # mini_chromium, without relaxing warnings in Snow's own code.
    set(_snow_mac_options " mac_deployment_target=\"${VCPKG_OSX_DEPLOYMENT_TARGET}\" extra_cflags=\"-Wno-error=deprecated-declarations\"")
    string(REPLACE "set(OPTIONS_DBG \"is_debug=true\")"
        "string(APPEND OPTIONS [==[${_snow_mac_options}]==])\nset(OPTIONS_DBG \"is_debug=true\")"
        _snow_port "${_snow_port}")
    string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}" "${_snow_upstream}" _snow_port "${_snow_port}")
    file(WRITE "${CURRENT_BUILDTREES_DIR}/snow-port.cmake" "${_snow_port}")
    include("${CURRENT_BUILDTREES_DIR}/snow-port.cmake")
    return()
endif()

set(_snow_escape [=[
    foreach(_snow_flags VCPKG_COMBINED_C_FLAGS_DEBUG VCPKG_COMBINED_CXX_FLAGS_DEBUG
            VCPKG_COMBINED_C_FLAGS_RELEASE VCPKG_COMBINED_CXX_FLAGS_RELEASE
            VCPKG_COMBINED_SHARED_LINKER_FLAGS_DEBUG VCPKG_COMBINED_SHARED_LINKER_FLAGS_RELEASE
            VCPKG_COMBINED_STATIC_LINKER_FLAGS_DEBUG VCPKG_COMBINED_STATIC_LINKER_FLAGS_RELEASE)
        # Only normalize the CRT flavor when this port is meant to be static-CRT:
        # with SNOW_CRASHPAD_DYNAMIC_CRT the /MD(d) flags the probe reports are
        # exactly what the resulting archives must carry.
        if(_snow_crashpad_static_crt)
            string(REPLACE "-MD" "-MT" ${_snow_flags} "${${_snow_flags}}")
            string(REPLACE "/MD" "/MT" ${_snow_flags} "${${_snow_flags}}")
        endif()
        string(REGEX REPLACE "[-/]GL([ ;]|$)" "/GL-\\1" ${_snow_flags} "${${_snow_flags}}")
        string(REPLACE "\\" "\\\\" ${_snow_flags} "${${_snow_flags}}")
        string(REPLACE "\"" "\\\"" ${_snow_flags} "${${_snow_flags}}")
    endforeach()
]=])
string(REPLACE "include(\"\${cmake_vars_file}\")"
    "include(\"\${cmake_vars_file}\")\n${_snow_escape}" _snow_port "${_snow_port}")
string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}" "${_snow_upstream}" _snow_port "${_snow_port}")
set(_snow_build_tools [=[
# Chromium's exception-disabled STL definitions must not participate in the
# application's exception-enabled LTCG compilation. Keep optimized native objects.
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/config/BUILD.gn"
    "\"/GL\"" "\"/GL-\"")
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/win_helper.py"
    "[vswhere_path, '-latest', '-property', 'installationPath']"
    "[vswhere_path, '-latest', '-products', '*', '-property', 'installationPath']")
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/win_helper.py"
    "    if arch == 'arm64':\n      script_arch_name = 'x86_arm64'"
    [==[    if script_path.endswith('vcvarsall.bat'):
      host_arch = (os.environ.get('SNOW_MSVC_HOST_ARCHITECTURE') or
                   os.environ.get('PROCESSOR_ARCHITEW6432') or
                   os.environ.get('PROCESSOR_ARCHITECTURE', 'AMD64')).lower()
      if host_arch in ('arm64', 'aarch64'):
        script_arch_name = {'x86': 'arm64_x86', 'amd64': 'arm64_x64',
                            'arm64': 'arm64'}[arch]
      else:
        script_arch_name = {'x86': 'amd64_x86', 'amd64': 'amd64',
                            'arm64': 'amd64_arm64'}[arch]
]==])
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/win_helper.py"
    "    args.extend((script_arch_name, '&&', 'set'))"
    [==[    args.append(script_arch_name)
    if script_path.endswith('vcvarsall.bat'):
      args.append('-vcvars_ver=14.51')
    args.extend(('&&', 'set'))]==])
]=])
string(REPLACE "vcpkg_gn_configure(" "${_snow_build_tools}\nvcpkg_gn_configure("
    _snow_port "${_snow_port}")
file(WRITE "${CURRENT_BUILDTREES_DIR}/snow-port.cmake" "${_snow_port}")
include("${CURRENT_BUILDTREES_DIR}/snow-port.cmake")
