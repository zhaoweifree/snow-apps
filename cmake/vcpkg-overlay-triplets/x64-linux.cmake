# UOS / Deepin x86_64 port triplet.
#
# The UOS port builds against distribution packages (docs-uos-linux-port.md §5.1),
# so the presets set VCPKG_MANIFEST_INSTALL=OFF and no toolchainFile is used.
# This triplet exists for the optional "vcpkg on Linux" route, where the handful
# of libraries UOS does not ship (libjxl, some OpenCV contrib modules, ...) may
# still be pulled in through the manifest. Keep it consistent with the system
# packages so the two sources can be mixed: dynamic libraries and the default
# glibc/libstdc++ (no static CRT concept on Linux).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# UOS 20 is glibc 2.28 (Debian 10 buster); building against newer symbols would
# make the resulting binaries refuse to load there. See docs-uos-linux-port.md
# appendix A.3, which treats the glibc baseline as the deciding constraint.
set(VCPKG_C_FLAGS "")
set(VCPKG_CXX_FLAGS "")
