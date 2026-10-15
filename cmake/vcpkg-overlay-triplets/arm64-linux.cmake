# UOS / Deepin aarch64 port triplet (Phytium / Kunpeng / Kirin 990).
#
# Native-first per docs-uos-linux-port.md §7.1: run the build on the arm64
# machine itself. The cross route (gcc-aarch64-linux-gnu) is a fallback only,
# and must be re-verified natively before it counts.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# NEON is baseline on armv8-a, so unlike the Windows AVX2 sources there is no
# need to opt individual translation units in; enabling it explicitly keeps the
# flag set identical to what SnowSimd.cmake will request once the NEON kernels
# land (docs-uos-linux-port.md §7.2).
set(VCPKG_C_FLAGS "-march=armv8-a+simd")
set(VCPKG_CXX_FLAGS "-march=armv8-a+simd")
