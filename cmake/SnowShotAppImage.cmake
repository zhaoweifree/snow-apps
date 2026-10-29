include_guard(GLOBAL)

# AppImage packaging for the Linux port (port document §9.5).
#
# CPack has no AppImage generator, so this is not a CPack module: it stages an
# AppDir with the ordinary install rules and then hands that directory to
# `appimagetool`. Split the same way as SnowShotLinuxPackage.cmake:
#
#  * `snow_shot_appimage_architecture()` and `snow_shot_appimage_layout()` are
#    pure functions -- no targets, no tools -- so
#    `scripts/check-linux-packaging.cmake` can evaluate them on a host that
#    cannot build the Linux target.
#  * `snow_shot_add_appimage_target()` is the only part that touches the build
#    graph, and the only part that needs `appimagetool`.

# Maps a processor name to the architecture spelling appimagetool expects.
#
# appimagetool ships type-2 runtime binaries for a fixed set: x86_64, i686,
# armhf, aarch64. loong64 and riscv64 are Debian targets this project builds for
# but AppImage cannot package; refusing here with the reason is better than
# emitting a file that no kernel will execute.
function(snow_shot_appimage_architecture out_var)
    cmake_parse_arguments(_snow "" "PROCESSOR" "" ${ARGN})
    if(NOT _snow_PROCESSOR)
        set(_snow_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_snow_PROCESSOR}" _snow_processor)
    if(_snow_processor MATCHES "^(x86_64|amd64)$")
        set(_snow_architecture "x86_64")
    elseif(_snow_processor MATCHES "^(aarch64|arm64)$")
        set(_snow_architecture "aarch64")
    elseif(_snow_processor MATCHES "^(loongarch64|loong64|riscv64|rv64|riscv64gc)$")
        message(FATAL_ERROR
            "snow_shot_appimage_architecture: '${_snow_PROCESSOR}' has no AppImage runtime. "
            "Use the DEB package for it, or extend this mapping once appimagetool ships a runtime.")
    else()
        message(FATAL_ERROR
            "snow_shot_appimage_architecture: unsupported processor '${_snow_PROCESSOR}'.")
    endif()
    set(${out_var} "${_snow_architecture}" PARENT_SCOPE)
endfunction()

# Computes the AppDir layout, the two files staged into it, and the output name.
#
# Returns, in the caller's scope:
#
#   SNOW_SHOT_APPIMAGE_ARCH            appimagetool's ARCH value
#   SNOW_SHOT_APPIMAGE_FILE_NAME       snow-shot-<version>-<arch>.AppImage
#   SNOW_SHOT_APPIMAGE_DIR             the staged AppDir (what gets deleted first)
#   SNOW_SHOT_APPIMAGE_USR             the AppDir's usr/ prefix, i.e. the install prefix
#   SNOW_SHOT_APPIMAGE_DESKTOP         the entry, as shipped by the install rules
#   SNOW_SHOT_APPIMAGE_DESKTOP_NAME    its file name, which is also the AppDir root name
#                                      (`app::edition::desktopEntryId()`, reverse-DNS)
#   SNOW_SHOT_APPIMAGE_ICON_NAME       the icon file name at the AppDir root
#   SNOW_SHOT_APPIMAGE_APPRUN_SOURCE   the reviewed launcher in the source tree
#   SNOW_SHOT_APPIMAGE_ICON_SOURCE     the 256x256 PNG staged as the root icon
#
# Where the source files live is an argument, not `${CMAKE_CURRENT_SOURCE_DIR}`.
# That variable is the directory of the CMakeLists.txt being processed, and the
# only caller lives in the root one -- so the two paths resolved to
# `<repo>/packaging/...` and `<repo>/resources/...`, neither of which exists.
# The configure that turned the option on died on the icon check and told the
# reader to generate icons that were already there. When APP_SOURCE_DIR is
# omitted the answer comes from the target itself, which always knows its own
# directory; the gate passes it explicitly so it can run in script mode.
#
# The version in the file name is the numeric triple, not `SNOW_SHOT_VERSION`:
# a pre-release suffix such as `-rc1` is legal in a file name but makes the
# name disagree with what the release tag claims.
function(snow_shot_appimage_layout)
    cmake_parse_arguments(_snow "" "APP_SOURCE_DIR" "" ${ARGN})
    snow_shot_appimage_architecture(_snow_architecture)
    if(_snow_APP_SOURCE_DIR)
        set(_snow_app_dir "${_snow_APP_SOURCE_DIR}")
    elseif(TARGET snow_shot)
        get_target_property(_snow_app_dir snow_shot SOURCE_DIR)
    else()
        message(FATAL_ERROR
            "snow_shot_appimage_layout: no APP_SOURCE_DIR and no snow_shot target. "
            "In script mode pass APP_SOURCE_DIR <path to snow_shot/>.")
    endif()
    set(_snow_dir "${CMAKE_BINARY_DIR}/appimage/snow_shot-${SNOW_SHOT_VERSION_NUMERIC}-${_snow_architecture}.AppDir")
    set(SNOW_SHOT_APPIMAGE_ARCH "${_snow_architecture}" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_FILE_NAME
        "snow-shot-${SNOW_SHOT_VERSION_NUMERIC}-${_snow_architecture}.AppImage" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_DIR "${_snow_dir}" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_USR "${_snow_dir}/usr" PARENT_SCOPE)
    # The entry name is reverse-DNS and comes from `app::edition::desktopEntryId()`;
    # the two copies below -- the install rule's and the AppDir root one -- have to
    # carry exactly that name. The gate reads this variable out of the layout
    # function, so the three spellings cannot drift apart without a red check.
    #
    # The local is separate from the exported name on purpose: `set(... PARENT_SCOPE)`
    # only writes to the caller, so the line below it would have expanded the
    # *previous* value of the variable -- nothing on a first call -- and produced
    # `.../applications/` with the file name missing.
    set(_snow_desktop_name "com.snowshot.snow_shot.desktop")
    set(SNOW_SHOT_APPIMAGE_DESKTOP_NAME "${_snow_desktop_name}" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_DESKTOP
        "${_snow_dir}/usr/share/applications/${_snow_desktop_name}" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_ICON_NAME "snow-shot.png" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_APPRUN_SOURCE
        "${_snow_app_dir}/packaging/linux/appimage/AppRun" PARENT_SCOPE)
    set(SNOW_SHOT_APPIMAGE_ICON_SOURCE
        "${_snow_app_dir}/resources/icons/hicolor/256x256/apps/snow-shot.png" PARENT_SCOPE)
endfunction()

# Attaches the `snow-shot-appimage` target.
#
# Ordering matters and is not expressible through DEPENDS alone: the AppDir is
# produced by `cmake --install`, which walks every install rule of the project,
# so the whole build has to be finished before it runs. Depending on the
# `snow_shot` target is the closest the build graph gets -- anything else that
# has an install rule of its own is built by `cmake --build` in the same
# invocation anyway.
function(snow_shot_add_appimage_target)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "snow_shot_add_appimage_target is Linux-only")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT TARGET snow_shot)
        message(FATAL_ERROR "SNOW_APPS_PACKAGE_APPIMAGE requires SNOW_APPS_BUILD_SNOW_SHOT=ON")
    endif()

    snow_shot_appimage_layout()

    # appimagetool, and the runtime it embeds, are fetched by hand on the target
    # machine (there is no distro package for them); the cache variable is the
    # supported way to point at a copy that is not on PATH.
    if(NOT SNOW_APPS_APPIMAGETOOL)
        find_program(SNOW_APPS_APPIMAGETOOL appimagetool HINTS "${CMAKE_SOURCE_DIR}/.tools/appimage")
    endif()
    if(NOT SNOW_APPS_APPIMAGETOOL)
        message(FATAL_ERROR
            "appimagetool not found. Install it, or configure with "
            "-DSNOW_APPS_APPIMAGETOOL=/path/to/appimagetool.")
    endif()

    # The maintained appimagetool does not bundle the type-2 runtime: it
    # downloads it live at packaging time unless one is supplied. A packaging
    # step that reaches the network on its own is neither hermetic nor
    # reproducible -- the runtime it would fetch is whatever upstream serves
    # that minute. So the runtime is passed explicitly.
    #
    # It stays optional because the historical AppImageKit build does bundle
    # one; a caller that only has that tool can leave the variable empty.
    set(_snow_appimage_runtime_args "")
    if(SNOW_APPS_APPIMAGETOOL_RUNTIME)
        if(NOT EXISTS "${SNOW_APPS_APPIMAGETOOL_RUNTIME}")
            message(FATAL_ERROR
                "SNOW_APPS_APPIMAGETOOL_RUNTIME does not exist: ${SNOW_APPS_APPIMAGETOOL_RUNTIME}")
        endif()
        set(_snow_appimage_runtime_args --runtime-file "${SNOW_APPS_APPIMAGETOOL_RUNTIME}")
    endif()

    set(_snow_apprun "${SNOW_SHOT_APPIMAGE_APPRUN_SOURCE}")
    set(_snow_icon_source "${SNOW_SHOT_APPIMAGE_ICON_SOURCE}")
    # Both checks are here rather than in `snow_shot_appimage_layout()` so the
    # gate can call the layout with a deliberately empty directory and still
    # compare the strings it computes.
    if(NOT EXISTS "${_snow_apprun}")
        message(FATAL_ERROR "Missing ${_snow_apprun}, the AppImage entry point.")
    endif()
    # appimagetool refuses an AppDir whose root icon is not exactly the name in
    # the entry's Icon=. The 256x256 PNG is the largest fixed size we ship, and
    # the scalable SVG next to it in the package is not what appimagetool reads.
    if(NOT EXISTS "${_snow_icon_source}")
        message(FATAL_ERROR
            "Missing ${_snow_icon_source}. Run: python3 scripts/gen-linux-icons.py")
    endif()
    # Checked here and not inside the custom target: an empty `-P` argument only
    # fails once the whole build has already run, and the message would be
    # `cmake -P: no file given`.
    if(NOT SNOW_SHOT_LINUX_BUNDLE_SCRIPT)
        message(FATAL_ERROR
            "snow_shot_add_appimage_target: snow_shot_configure_linux_package() has "
            "to run first; it generates the script that puts the private libraries "
            "into the AppDir.")
    endif()

    add_custom_target(snow-shot-appimage
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SNOW_SHOT_APPIMAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${SNOW_SHOT_APPIMAGE_USR}"
                --config "$<CONFIG>"
        # The AppDir is staged by `cmake --install`, and CPack -- the only other
        # thing that knows how to fill the private library directory -- is not
        # part of this path. So the same script CPack runs as an install script
        # is run here by hand, on the prefix that was just filled.
        #
        # Skipping it produced an AppDir with the executable, the entry, the icon
        # and no shared library at all: the executable's RUNPATH points at
        # `../lib/snow-shot`, which would have been empty, and
        # `check-linux-appimage-build.py` asks for the bundled libraries because
        # an AppImage whose `libjxl` is missing is not a product.
        COMMAND "${CMAKE_COMMAND}" "-DSTAGED_DIR=${SNOW_SHOT_APPIMAGE_USR}"
                -P "${SNOW_SHOT_LINUX_BUNDLE_SCRIPT}"
        # `-E copy` carries the source's mode across: `_cmake_E_copy` calls
        # `cmSystemTools::CopyFileAlways`, which reads `GetPermissions(source)`
        # and applies `SetPermissions(destination, perm)` afterwards. AppRun is
        # 100755 in the index, so the copy is executable -- and
        # `check-linux-appimage-build.py` verifies that on the unpacked image.
        #
        # There is no `cmake -E chmod`. This chain used to end with one, and run
        # 43 -- the first run that ever reached this target -- found out:
        #
        #     CMake Error: cmake version 4.4.4
        #     Usage: cmake -E <command> [arguments...]
        #
        # followed by the 40-line command list, in which `chmod` does not
        # appear. The copy above already did the job; that command could only
        # ever fail.
        COMMAND "${CMAKE_COMMAND}" -E copy "${_snow_apprun}" "${SNOW_SHOT_APPIMAGE_DIR}/AppRun"
        COMMAND "${CMAKE_COMMAND}" -E copy "${SNOW_SHOT_APPIMAGE_DESKTOP}"
                "${SNOW_SHOT_APPIMAGE_DIR}/${SNOW_SHOT_APPIMAGE_DESKTOP_NAME}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_snow_icon_source}"
                "${SNOW_SHOT_APPIMAGE_DIR}/${SNOW_SHOT_APPIMAGE_ICON_NAME}"
        # appimagetool sizes the AppImage window thumbnail from .DirIcon; a
        # missing one only shows up as a blank icon in the file manager.
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${SNOW_SHOT_APPIMAGE_ICON_NAME}" "${SNOW_SHOT_APPIMAGE_DIR}/.DirIcon"
        COMMAND "${CMAKE_COMMAND}" -E env
                "ARCH=${SNOW_SHOT_APPIMAGE_ARCH}"
                # Without FUSE -- containers, CI runners -- appimagetool refuses
                # to start unless it is told to extract its own runtime instead.
                "APPIMAGE_EXTRACT_AND_RUN=1"
                "${SNOW_APPS_APPIMAGETOOL}" ${_snow_appimage_runtime_args}
                "${SNOW_SHOT_APPIMAGE_DIR}"
                "${CMAKE_BINARY_DIR}/${SNOW_SHOT_APPIMAGE_FILE_NAME}"
        COMMENT "Staging the AppDir and packaging ${SNOW_SHOT_APPIMAGE_FILE_NAME}"
        VERBATIM)

    add_dependencies(snow-shot-appimage snow_shot)

    # The executable has to find the bundled libraries through its own location
    # once the AppImage is mounted somewhere the loader never heard of. AppRun
    # sets LD_LIBRARY_PATH as well; the RPATH is what still works if the binary
    # is ever run out of the AppDir without going through AppRun.
    #
    # The layout is not recomputed here: `snow_shot_apply_linux_private_rpath()`
    # (SnowShotLinuxPackage.cmake) already points every installed ELF object at
    # the private library directory, and the AppDir is staged from the same
    # install rules. Setting a different RPATH here -- this used to say
    # `$ORIGIN/../lib`, which is the directory *above* the private one -- would
    # silently undo it, because this call runs last.
    if(NOT COMMAND snow_shot_apply_linux_private_rpath)
        message(FATAL_ERROR
            "snow_shot_add_appimage_target: the Linux packaging module has to be "
            "included first; it owns the private library directory and the RPATH.")
    endif()
    snow_shot_apply_linux_private_rpath()
endfunction()
