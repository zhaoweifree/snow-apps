include_guard(GLOBAL)

# Debian packaging for the Linux port (port document §9.5).
#
# Two parts, deliberately separated:
#
#  * `snow_shot_linux_debian_architecture()` is a pure mapping and can be
#    evaluated anywhere, including on a machine that cannot build the Linux
#    target at all. `scripts/check-linux-packaging.cmake` exercises it.
#  * `snow_shot_configure_linux_package()` only *assigns* CPack variables in the
#    caller's scope. It does not call `include(CPack)`: keeping that call in the
#    top-level file is what lets the harness above run the whole configuration
#    without a toolchain, and it keeps the "one include(CPack) per project"
#    invariant that CPack itself expects.

# Maps a processor name to the architecture string `dpkg` accepts.
#
# This is not cosmetic: the DEB generator writes whatever it is given into
# `Architecture:` and dpkg then refuses the package on a matching machine
# (`x86_64` is not an architecture, `amd64` is). Unknown names fail loudly here
# instead of producing a package that cannot be installed anywhere.
function(snow_shot_linux_debian_architecture out_var)
    cmake_parse_arguments(_snow "" "PROCESSOR" "" ${ARGN})
    if(NOT _snow_PROCESSOR)
        set(_snow_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_snow_PROCESSOR}" _snow_processor)
    if(_snow_processor MATCHES "^(x86_64|amd64)$")
        set(_snow_debian_architecture "amd64")
    elseif(_snow_processor MATCHES "^(aarch64|arm64)$")
        set(_snow_debian_architecture "arm64")
    elseif(_snow_processor MATCHES "^(loongarch64|loong64)$")
        set(_snow_debian_architecture "loong64")
    elseif(_snow_processor MATCHES "^(mips64el|mips64)$")
        set(_snow_debian_architecture "mips64el")
    elseif(_snow_processor MATCHES "^(riscv64|rv64|riscv64gc)$")
        set(_snow_debian_architecture "riscv64")
    else()
        message(FATAL_ERROR
            "snow_shot_linux_debian_architecture: unsupported processor '${_snow_PROCESSOR}'. "
            "Add the Debian architecture name before packaging for it.")
    endif()
    set(${out_var} "${_snow_debian_architecture}" PARENT_SCOPE)
endfunction()

# The directory the package keeps its own shared libraries in, relative to the
# install prefix.
#
# Private, not `${CMAKE_INSTALL_LIBDIR}` directly: these are pinned versions
# (`libjxl.so.0.11`, `libImath-3_2.so.30`) that a distribution package may also
# provide in a different version, and two packages owning the same `/usr/lib`
# path is a packaging conflict. The RPATH below is what makes the private copy
# reachable, so the directory name is a decision both that RPATH and the bundle
# script read from here.
#
# Reads the cached `CMAKE_INSTALL_LIBDIR`, which `snow_shot_configure_linux_package()`
# fills by including `GNUInstallDirs` itself (script mode never runs the include in
# `snow_shot/CMakeLists.txt`, and an empty value here returns an absolute
# `/snow-shot` that every consumer accepts without complaint). No include of its
# own: the cache makes it a no-op, and a no-op cannot be reverse-verified.
function(snow_shot_linux_private_lib_dir out_var)
    set(${out_var} "${CMAKE_INSTALL_LIBDIR}/snow-shot" PARENT_SCOPE)
endfunction()

# Finds the directory the pinned dependencies' shared libraries were installed
# into.
#
# Guessing `${prefix}/lib` is not enough: `GNUInstallDirs` picks `lib64` or
# `lib/<triplet>` depending on the host and on what the library's own build
# system decided, and the first version of this function failed exactly that way
# on the runner ("no dependency prefix in CMAKE_PREFIX_PATH") while OpenCV was
# being found in that same prefix two lines earlier.
#
# So the probe is a *library*, found recursively: `lunasvg` is one of the pinned
# builds and no distribution package provides it, which makes it a reliable
# marker for "the tree scripts/build-linux-deps.sh installed".
function(snow_shot_linux_find_deps_lib_dir out_var)
    set(_found "")
    set(_searched "")
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _searched "${_prefix}")
        file(GLOB_RECURSE _probe
            "${_prefix}/lib*/liblunasvg.so*"
            "${_prefix}/lib*/libjxl.so*")
        if(_probe)
            list(GET _probe 0 _first)
            get_filename_component(_found "${_first}" DIRECTORY)
            break()
        endif()
    endforeach()
    if(NOT _found AND EXISTS "$ENV{CMAKE_PREFIX_PATH}")
        set(_env_prefix "$ENV{CMAKE_PREFIX_PATH}")
        file(GLOB_RECURSE _probe "${_env_prefix}/lib*/liblunasvg.so*")
        if(_probe)
            list(GET _probe 0 _first)
            get_filename_component(_found "${_first}" DIRECTORY)
        endif()
        list(APPEND _searched "env:${_env_prefix}")
    endif()
    set(SNOW_SHOT_LINUX_DEPS_SEARCHED "${_searched}" PARENT_SCOPE)
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# Points every installed ELF object at the private library directory.
#
# Without this the loader searches only the system paths, so the private copies
# are invisible at run time -- and `CPackDeb.cmake` never learns about them
# either: it turns `$ORIGIN` in a binary's RUNPATH into `-l<dir>` for
# `dpkg-shlibdeps`, and a binary with no RUNPATH contributes nothing, which is
# how the first packaged build failed with `cannot find library libjxl.so.0.11`.
#
# Executables live in `bin/` and libraries in `lib/`, so the relative hop is not
# the same for both. `INSTALL_RPATH_USE_LINK_PATH OFF` keeps the build tree's
# own library paths out of the installed binary.
function(snow_shot_apply_linux_private_rpath)
    # `CMAKE_INSTALL_LIBDIR` comes from the cache that
    # `snow_shot_configure_linux_package()` fills; the AppImage module calls this
    # after it, by construction.
    snow_shot_linux_private_lib_dir(_private)
    get_filename_component(_private_name "${_private}" NAME)
    foreach(_target IN ITEMS
            snow_shot
            snow_ocr_process
            snow_shot_mcp
            snow_shot_image_codec_backend)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            set(_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/${_private_name}")
        else()
            set(_rpath "$ORIGIN/${_private_name}")
        endif()
        set_target_properties(${_target} PROPERTIES
            INSTALL_RPATH "${_rpath}"
            INSTALL_RPATH_USE_LINK_PATH OFF)
    endforeach()
endfunction()

# Assigns every CPACK_* variable the DEB layout needs.
#
# `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is the important one. The alternative -- a
# hand-written `Depends:` list -- is a guess that goes stale silently: a Qt
# module added to the link line later would produce a package that installs and
# then fails to start with "error while loading shared libraries". Letting CPack
# run `dpkg-shlibdeps` over the staged binaries derives the list from what the
# executable actually links against, which is the only version that stays true.
function(snow_shot_configure_linux_package)
    # `GNUInstallDirs` caches `CMAKE_INSTALL_<dir>`, so a real configure sees the
    # values from `snow_shot/CMakeLists.txt` here through the cache. Script mode
    # (`cmake -P`, i.e. scripts/check-linux-packaging.cmake) never ran that
    # include, so the values were empty there and everything below degraded
    # silently: the private library directory became `/snow-shot` (absolute,
    # outside the prefix), the RPATH lost its `lib` hop, and the substituted
    # bindir became the empty string -- which turned the staged glob into
    # `${_staged}//*`, the staging *root*, whose only entries are directories.
    # Including it here makes the gate see exactly what a configure sees.
    include(GNUInstallDirs)
    # Script mode (`cmake -P`) has no targets at all, which is how
    # scripts/check-linux-packaging.cmake evaluates this function on a host that
    # cannot build the Linux target. The guard only exists for real configure
    # runs, where a missing target means the caller forgot to build snow_shot.
    if(NOT TARGET snow_shot AND NOT CMAKE_SCRIPT_MODE_FILE)
        message(FATAL_ERROR "SNOW_APPS_PACKAGE_SNOW_SHOT requires SNOW_APPS_BUILD_SNOW_SHOT=ON")
    endif()

    snow_shot_linux_debian_architecture(_snow_debian_architecture)

    set(CPACK_PACKAGE_NAME "snow-shot")
    set(CPACK_PACKAGE_VENDOR "${SNOW_SHOT_VENDOR}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${SNOW_SHOT_DESCRIPTION}")
    set(CPACK_PACKAGE_DESCRIPTION "${SNOW_SHOT_PRODUCT_NAME} ${SNOW_SHOT_VERSION}\n\
${SNOW_SHOT_DESCRIPTION}\n\
\n\
Capture a region, a window or a scrolling page, annotate the result, pin it\n\
above other windows, and recognize text and tables from the same image.\n")
    set(CPACK_PACKAGE_HOMEPAGE_URL "${SNOW_SHOT_HOMEPAGE_URL}")
    set(CPACK_PACKAGE_CONTACT "${SNOW_SHOT_VENDOR} <${SNOW_SHOT_SUPPORT_EMAIL}>")
    # Debian versions are not always semver: the pre-release suffix of
    # `1.3.0-rc1` has to become `~rc1` or dpkg sorts it *after* 1.3.0. Only the
    # numeric triple is safe to pass through unchanged, and that is what the
    # version variables this project defines already provide.
    set(CPACK_PACKAGE_VERSION "${SNOW_SHOT_VERSION_NUMERIC}")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_PACKAGE_CHECKSUM "SHA256")
    set(CPACK_PACKAGE_EXECUTABLES "${SNOW_SHOT_EXECUTABLE_NAME}" "${SNOW_SHOT_PRODUCT_NAME}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "SnowShot")
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};SnowShot;/")
    set(CPACK_COMPONENTS_ALL SnowShot)
    # CPack joins list-valued variables with the list separator; without this the
    # `;` in a dependency path would be taken literally.
    set(CPACK_VERBATIM_VARIABLES ON)

    set(CPACK_GENERATOR "DEB")
    # FHS layout: /usr/bin/snow_shot, /usr/share/applications, /usr/share/icons.
    # The default (/usr/local) would install a package that no launcher reads.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_NAME "snow-shot")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_snow_debian_architecture}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${SNOW_SHOT_VENDOR} <${SNOW_SHOT_SUPPORT_EMAIL}>")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${SNOW_SHOT_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # Refuse to ship a package whose file modes depend on the umask of whoever
    # ran CPack.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

    # --- the project's own shared libraries ---------------------------------
    # `SHLIBDEPS=ON` is only usable if every NEEDED soname either belongs to an
    # installed distribution package or is already inside the package. The
    # pinned dependencies are self-built (`libjxl.so.0.11`,
    # `libOpenEXR-3_4.so.33`, `liblunasvg.so.3` ...), and no distribution
    # package owns them, so `dpkg-shlibdeps` refuses:
    #
    #     dpkg-shlibdeps: error: cannot find library libjxl.so.0.11 needed by ...
    #
    # Three things have to agree for the package to be both buildable and
    # runnable, which is why they are set together here:
    #
    #   1. the private libraries are copied into the staged tree
    #      (`cmake/SnowShotLinuxRuntimeBundle.cmake.in`, run by CPack);
    #   2. the installed ELF objects carry an RPATH that reaches them
    #      (`snow_shot_apply_linux_private_rpath()`);
    #   3. `SHLIBDEPS_PRIVATE_DIRS` names the build prefix, so the copies can be
    #      resolved even before the RPATH hop is taken.
    snow_shot_linux_find_deps_lib_dir(_snow_deps_lib)
    snow_shot_linux_private_lib_dir(_snow_private_lib)
    if(NOT _snow_deps_lib)
        if(CMAKE_SCRIPT_MODE_FILE)
            # The gate evaluates this function on a workstation that has no
            # dependency prefix. The variables it checks are still assigned; the
            # generated script reports the missing prefix if CPack ever runs.
            message(STATUS
                "snow-shot linux package: no dependency prefix in CMAKE_PREFIX_PATH "
                "(script mode); the bundle script will refuse to run")
        else()
            message(FATAL_ERROR
                "SNOW_APPS_PACKAGE_SNOW_SHOT needs the directory the pinned "
                "dependencies were installed into, and no CMAKE_PREFIX_PATH entry "
                "contains liblunasvg.so / libjxl.so. Searched: "
                "${SNOW_SHOT_LINUX_DEPS_SEARCHED}. Without it the self-built "
                "libraries cannot be bundled and dpkg-shlibdeps fails on the first "
                "one it cannot find.")
        endif()
    endif()

    set(_snow_bundle_script "${CMAKE_BINARY_DIR}/snow-shot-linux-runtime-bundle.cmake")
    # The AppImage target runs the same script on its own prefix (see
    # `snow_shot_add_appimage_target()`), so the path has to leave this function.
    set(SNOW_SHOT_LINUX_BUNDLE_SCRIPT "${_snow_bundle_script}")
    set(SNOW_SHOT_LINUX_DEPS_LIB_DIR "${_snow_deps_lib}")
    set(SNOW_SHOT_LINUX_PRIVATE_LIB_DIR "${_snow_private_lib}")
    set(SNOW_SHOT_LINUX_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(SNOW_SHOT_LINUX_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
    # The bundle script has to tell "a directory on this machine" from "the prefix
    # inside the package", and the only way to do that is to know the latter.
    set(SNOW_SHOT_LINUX_PACKAGE_PREFIX "${CPACK_PACKAGING_INSTALL_PREFIX}")
    configure_file(
        # Not `${CMAKE_CURRENT_LIST_DIR}`: inside a function body that variable is
        # the directory of the *caller's* listfile, so the template resolved to
        # `scripts/SnowShotLinuxRuntimeBundle.cmake.in` when the packaging gate
        # called this function (port document §9.6.10 is the same mistake with
        # `include()`). `CMAKE_CURRENT_FUNCTION_LIST_DIR` is the file the
        # function was defined in, wherever it is called from.
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SnowShotLinuxRuntimeBundle.cmake.in"
        "${_snow_bundle_script}"
        @ONLY)
    # An install rule, not `CPACK_INSTALL_SCRIPTS`. CPack runs those scripts
    # *before* it installs the project -- run 38's staging tree was empty when
    # the script ran, and it refused with "the staged tree has no ELF objects".
    # It also has no effect on `cmake --install`, which is how the AppImage
    # stages its AppDir. This rule runs after the install rules above it, which
    # include every file the package ships, and it runs for both consumers.
    #
    # Guarded because `install()` is not scriptable: the packaging gate calls
    # this function in `cmake -P` mode to inspect the values it assigns.
    #
    # `COMPONENT` is not decoration. CPack is told to install the component
    # `SnowShot` (`CPACK_INSTALL_CMAKE_PROJECTS`), and a rule with no component
    # falls back to `CMAKE_INSTALL_DEFAULT_COMPONENT_NAME` -- which the project
    # sets inside `snow_shot/CMakeLists.txt`, a *directory* scope that does not
    # reach this one. Without the argument this rule would be `Unspecified` and
    # silently skipped for the .deb while still running for `cmake --install`.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        install(SCRIPT "${_snow_bundle_script}" COMPONENT "${CPACK_COMPONENTS_ALL}")
    endif()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${_snow_deps_lib}")
    snow_shot_apply_linux_private_rpath()

    foreach(_snow_variable
            CPACK_PACKAGE_NAME
            CPACK_PACKAGE_VENDOR
            CPACK_PACKAGE_DESCRIPTION_SUMMARY
            CPACK_PACKAGE_DESCRIPTION
            CPACK_PACKAGE_HOMEPAGE_URL
            CPACK_PACKAGE_CONTACT
            CPACK_PACKAGE_VERSION
            CPACK_PACKAGE_DIRECTORY
            CPACK_PACKAGE_CHECKSUM
            CPACK_PACKAGE_EXECUTABLES
            CPACK_PACKAGE_INSTALL_DIRECTORY
            CPACK_INSTALL_CMAKE_PROJECTS
            CPACK_COMPONENTS_ALL
            CPACK_VERBATIM_VARIABLES
            CPACK_GENERATOR
            CPACK_PACKAGING_INSTALL_PREFIX
            CPACK_DEBIAN_FILE_NAME
            CPACK_DEBIAN_PACKAGE_NAME
            CPACK_DEBIAN_PACKAGE_ARCHITECTURE
            CPACK_DEBIAN_PACKAGE_SECTION
            CPACK_DEBIAN_PACKAGE_PRIORITY
            CPACK_DEBIAN_PACKAGE_MAINTAINER
            CPACK_DEBIAN_PACKAGE_HOMEPAGE
            CPACK_DEBIAN_PACKAGE_SHLIBDEPS
            CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
            CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION
            SNOW_SHOT_LINUX_BUNDLE_SCRIPT)
        set(${_snow_variable} "${${_snow_variable}}" PARENT_SCOPE)
    endforeach()
endfunction()
