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

# Assigns every CPACK_* variable the DEB layout needs.
#
# `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is the important one. The alternative -- a
# hand-written `Depends:` list -- is a guess that goes stale silently: a Qt
# module added to the link line later would produce a package that installs and
# then fails to start with "error while loading shared libraries". Letting CPack
# run `dpkg-shlibdeps` over the staged binaries derives the list from what the
# executable actually links against, which is the only version that stays true.
function(snow_shot_configure_linux_package)
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
            CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION)
        set(${_snow_variable} "${${_snow_variable}}" PARENT_SCOPE)
    endforeach()
endfunction()
