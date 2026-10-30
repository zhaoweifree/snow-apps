cmake_minimum_required(VERSION 3.22)
project(SnowX265ArmIntrinsics LANGUAGES C CXX)

set(SNOW_X265_SOURCE "" CACHE PATH "Patched x265 4.1 source tree")
option(SNOW_X265_HIGH_BIT_DEPTH "Validate Main10 instead of 8-bit" OFF)
option(SNOW_X265_MSVC_ASSEMBLY "Validate the opt-in Windows ARMASM64 backend" OFF)
set(SNOW_X265_MAIN10_ARCHIVE "" CACHE FILEPATH "Prebuilt EXPORT_C_API=OFF Main10 static archive")
if(NOT EXISTS "${SNOW_X265_SOURCE}/source/CMakeLists.txt")
    message(FATAL_ERROR "SNOW_X265_SOURCE must contain patched x265 4.1 sources")
endif()
set(ENABLE_ASSEMBLY "${SNOW_X265_MSVC_ASSEMBLY}" CACHE BOOL "" FORCE)
set(ENABLE_MSVC_ARM64_ASSEMBLY "${SNOW_X265_MSVC_ASSEMBLY}" CACHE BOOL "" FORCE)
set(ENABLE_ARM_INTRINSICS ON CACHE BOOL "" FORCE)
set(ENABLE_NEON_DOTPROD OFF CACHE BOOL "" FORCE)
set(ENABLE_NEON_I8MM OFF CACHE BOOL "" FORCE)
set(ENABLE_SVE OFF CACHE BOOL "" FORCE)
set(ENABLE_SVE2 OFF CACHE BOOL "" FORCE)
set(ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS ON CACHE BOOL "" FORCE)
set(ENABLE_LIBNUMA OFF CACHE BOOL "" FORCE)
set(ENABLE_PIC ON CACHE BOOL "" FORCE)
set(HIGH_BIT_DEPTH "${SNOW_X265_HIGH_BIT_DEPTH}" CACHE BOOL "" FORCE)
set(MAIN12 OFF CACHE BOOL "" FORCE)
set(EXPORT_C_API ON CACHE BOOL "" FORCE)
set(EXTRA_LIB "${SNOW_X265_MAIN10_ARCHIVE}" CACHE STRING "" FORCE)
if(SNOW_X265_MAIN10_ARCHIVE)
    if(SNOW_X265_HIGH_BIT_DEPTH OR NOT EXISTS "${SNOW_X265_MAIN10_ARCHIVE}")
        message(FATAL_ERROR "Combined API requires an 8-bit build and an existing private Main10 archive")
    endif()
    set(LINKED_10BIT ON CACHE BOOL "" FORCE)
else()
    set(LINKED_10BIT OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${SNOW_X265_SOURCE}/source" x265)

add_executable(x265-arm-intrinsics-dispatch-tests intrinsics_tests.cpp)
target_compile_features(x265-arm-intrinsics-dispatch-tests PRIVATE cxx_std_17)
target_include_directories(x265-arm-intrinsics-dispatch-tests PRIVATE
    "${SNOW_X265_SOURCE}/source" "${SNOW_X265_SOURCE}/source/common"
    "${CMAKE_CURRENT_BINARY_DIR}/x265")
if(SNOW_X265_HIGH_BIT_DEPTH)
    target_compile_definitions(x265-arm-intrinsics-dispatch-tests PRIVATE HIGH_BIT_DEPTH=1 X265_DEPTH=10)
else()
    target_compile_definitions(x265-arm-intrinsics-dispatch-tests PRIVATE HIGH_BIT_DEPTH=0 X265_DEPTH=8)
endif()
target_compile_definitions(x265-arm-intrinsics-dispatch-tests PRIVATE
    X265_NS=x265 X265_ARCH_ARM64=1 HAVE_NEON=1 HAVE_INT_TYPES_H=1)
if(SNOW_X265_MSVC_ASSEMBLY)
    target_compile_definitions(x265-arm-intrinsics-dispatch-tests PRIVATE SNOW_X265_TEST_ASSEMBLY=1)
endif()
if(SNOW_X265_MAIN10_ARCHIVE)
    target_compile_definitions(x265-arm-intrinsics-dispatch-tests PRIVATE SNOW_X265_TEST_MAIN10=1)
endif()
find_package(Threads REQUIRED)
target_link_libraries(x265-arm-intrinsics-dispatch-tests PRIVATE x265-static Threads::Threads ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME x265-arm-intrinsics-dispatch-tests COMMAND x265-arm-intrinsics-dispatch-tests)
foreach(bench IN ITEMS pixel transforms interp intrapred)
    add_test(NAME x265-arm-intrinsics-${bench}-tests
        COMMAND TestBench --cpuid NEON --testbench ${bench} --nobench)
endforeach()
add_test(NAME x265-arm-bitstream-scalar-tests
    COMMAND x265-arm-intrinsics-dispatch-tests scalar "${CMAKE_CURRENT_BINARY_DIR}/scalar.hevc")
add_test(NAME x265-arm-bitstream-neon-tests
    COMMAND x265-arm-intrinsics-dispatch-tests neon "${CMAKE_CURRENT_BINARY_DIR}/neon.hevc")
add_test(NAME x265-arm-bitstream-equality-tests
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${CMAKE_CURRENT_BINARY_DIR}/scalar.hevc" "${CMAKE_CURRENT_BINARY_DIR}/neon.hevc")
set_tests_properties(x265-arm-bitstream-equality-tests PROPERTIES
    DEPENDS "x265-arm-bitstream-scalar-tests;x265-arm-bitstream-neon-tests")
if(SNOW_X265_MAIN10_ARCHIVE)
    add_test(NAME x265-arm-combined-api-tests
        COMMAND x265-arm-intrinsics-dispatch-tests --combined-api)
    add_test(NAME x265-arm-combined-main10-scalar-tests
        COMMAND x265-arm-intrinsics-dispatch-tests scalar "${CMAKE_CURRENT_BINARY_DIR}/main10-scalar.hevc" 10)
    add_test(NAME x265-arm-combined-main10-neon-tests
        COMMAND x265-arm-intrinsics-dispatch-tests neon "${CMAKE_CURRENT_BINARY_DIR}/main10-neon.hevc" 10)
    add_test(NAME x265-arm-combined-main10-equality-tests
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${CMAKE_CURRENT_BINARY_DIR}/main10-scalar.hevc" "${CMAKE_CURRENT_BINARY_DIR}/main10-neon.hevc")
    set_tests_properties(x265-arm-combined-main10-equality-tests PROPERTIES
        DEPENDS "x265-arm-combined-main10-scalar-tests;x265-arm-combined-main10-neon-tests")
endif()
