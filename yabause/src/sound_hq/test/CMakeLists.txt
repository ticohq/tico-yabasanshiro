# Unit tests for the SCSP high-quality renderer (sound_hq/scsp_hq.cpp) and
# the float effect DSP (sound_hq/dsp_float.cpp, against the chip's scspdsp.c).
#   cmake -S yabause/src/sound_hq/test -B yabause/src/sound_hq/test/build
#   cmake --build yabause/src/sound_hq/test/build --config Release --target scsp_hq_test scsp_hq_dsp_test
#   yabause/src/sound_hq/test/build/Release/scsp_hq_test.exe [--gtest_filter=...]
#   yabause/src/sound_hq/test/build/Release/scsp_hq_dsp_test.exe
# Pass -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<googletest source> to build offline
# (for example the copy under yabause/src/vulkan/test/build/_deps/googletest-src).
cmake_minimum_required(VERSION 3.14)
project(scsp_hq_test C CXX)

# googletest needs C++14; the renderer itself is built as C++11, as the
# core's non-MSVC builds compile it (-std=c++11 in yabause/src/CMakeLists.txt).
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

set(_vk_gtest "${CMAKE_CURRENT_SOURCE_DIR}/../../vulkan/test/build/_deps/googletest-src")
if(NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST AND EXISTS "${_vk_gtest}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${_vk_gtest}")
endif()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(scsp_hq STATIC ../scsp_hq.cpp ../dsp_float.cpp)
set_target_properties(scsp_hq PROPERTIES CXX_STANDARD 11 CXX_EXTENSIONS OFF)
target_include_directories(scsp_hq PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(scsp_hq_test hq_test.cpp)
target_link_libraries(scsp_hq_test scsp_hq GTest::gtest_main)

# The chip's DSP, compiled in as the reference for the float DSP.
add_library(chip_dsp STATIC ../../scspdsp.c)
target_include_directories(chip_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_definitions(chip_dsp PUBLIC HAVE_C99_VARIADIC_MACROS _CRT_SECURE_NO_WARNINGS)
add_executable(scsp_hq_dsp_test dsp_test.cpp)
target_link_libraries(scsp_hq_dsp_test scsp_hq chip_dsp GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(scsp_hq_test)
gtest_discover_tests(scsp_hq_dsp_test)
