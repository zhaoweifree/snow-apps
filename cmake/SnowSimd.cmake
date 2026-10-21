include_guard(GLOBAL)

# AArch64 deliberately has no analogue of the function below.
#
# ARMv8-A mandates Advanced SIMD, so unlike x86 there is no "least common
# denominator" build where NEON has to be switched on per translation unit:
# every aarch64 compiler enables it at -O0 already. Adding `-march=armv8-a+simd`
# would only pin the baseline back to v8.0 on machines that could use v8.2
# (dot-product, FP16), which is the opposite of what a SIMD helper is for.
#
# The pixel-format hot path is not C++ either: it lives in
# `snow-crates/crates/snow-capture/src/convert/simd_neon.rs`, where
# `#[target_feature(enable = "neon")]` selects the kernel per function and
# runtime dispatch mirrors the x86 tiers. The C++ `*_avx2.cpp` translation
# units that do exist are each wrapped in `#if defined(__x86_64__) || ...`, so
# on aarch64 they compile to "unsupported" stubs and their callers fall back to
# scalar -- verified by the aarch64 cross-check, not assumed.

# Keep runtime-dispatched AVX2 kernels separate from baseline translation units.
# On macOS, CMAKE_SYSTEM_PROCESSOR can describe the host during a cross build.
function(snow_configure_avx2_sources)
    set(_snow_avx2_options)
    if(MSVC)
        if(SNOW_WINDOWS_ARCHITECTURE STREQUAL "x64")
            set(_snow_avx2_options "/arch:AVX2")
        endif()
    elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
        if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
            list(LENGTH CMAKE_OSX_ARCHITECTURES _snow_architecture_count)
            if(_snow_architecture_count EQUAL 1)
                set(_snow_avx2_options "-mavx2")
            else()
                # Apply the option only to the x64 slice of a universal build.
                set(_snow_avx2_options "-Xarch_x86_64" "-mavx2")
            endif()
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|i.86)$")
        set(_snow_avx2_options "-mavx2")
    endif()

    if(_snow_avx2_options)
        set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${_snow_avx2_options})
        set_source_files_properties(${ARGN} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Clang cannot load a baseline-CPU PCH in an AVX2 translation unit.
            set_source_files_properties(${ARGN} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endif()
endfunction()
