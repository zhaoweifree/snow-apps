cmake_minimum_required(VERSION 3.25)
project(SnowX264PreallocationContract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SNOW_X264_ROOT "" CACHE PATH "Repository-managed x264 installation to verify.")
find_path(SNOW_X264_INCLUDE_DIR x264.h
    PATHS "${SNOW_X264_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(SNOW_X264_LIBRARY NAMES x264 libx264
    PATHS "${SNOW_X264_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_package(Threads REQUIRED)

add_executable(snow-x264-preallocation-tests preallocation_tests.cpp)
target_include_directories(snow-x264-preallocation-tests PRIVATE "${SNOW_X264_INCLUDE_DIR}")
target_link_libraries(snow-x264-preallocation-tests PRIVATE
    "${SNOW_X264_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
    target_link_libraries(snow-x264-preallocation-tests PRIVATE m)
endif()
if(APPLE)
    # Exercise the production archive with the same optimized link that exposed
    # invalid pointer aliasing in x264's shared frame/macroblock allocator.
    target_compile_options(snow-x264-preallocation-tests PRIVATE -O3 -flto)
    target_link_options(snow-x264-preallocation-tests PRIVATE -flto)
endif()

enable_testing()
foreach(_depth IN ITEMS 8 10)
    add_test(NAME snow-x264-preallocation-${_depth}bit-tests
        COMMAND snow-x264-preallocation-tests ${_depth})
    set_tests_properties(snow-x264-preallocation-${_depth}bit-tests PROPERTIES
        LABELS unit TIMEOUT 60)
endforeach()
