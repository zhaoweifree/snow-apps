cmake_minimum_required(VERSION 3.25)
project(SnowClipboardDibContract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(_snow_dib_source "${CMAKE_CURRENT_SOURCE_DIR}/../../snow_shot/src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SnowSimd.cmake")
snow_configure_avx2_sources("${_snow_dib_source}")
add_executable(snow-shot-clipboard-dib-decoder-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../snow_shot/tests/clipboard_dib_decoder_tests.cpp"
    "${_snow_dib_source}")
set_target_properties(snow-shot-clipboard-dib-decoder-tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
option(SNOW_DIB_ENABLE_UBSAN "Check DIB byte access with UndefinedBehaviorSanitizer." OFF)
if(SNOW_DIB_ENABLE_UBSAN)
    target_compile_options(snow-shot-clipboard-dib-decoder-tests PRIVATE
        -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(snow-shot-clipboard-dib-decoder-tests PRIVATE
        -fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
enable_testing()
add_test(NAME snow-shot-clipboard-dib-decoder-tests COMMAND snow-shot-clipboard-dib-decoder-tests)
set_tests_properties(snow-shot-clipboard-dib-decoder-tests PROPERTIES LABELS unit TIMEOUT 30)
