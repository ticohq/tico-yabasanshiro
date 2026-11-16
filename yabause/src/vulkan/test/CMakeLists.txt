cmake_minimum_required(VERSION 3.14)
project(vdp1_compute_math_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable CTest so gtest_discover_tests() emits a top-level CTestTestfile.cmake
# and `ctest` can run the suites. Test executables can still be run directly.
enable_testing()

# -------------------------------------------------------
# GLM: use vendored copy from the main build's FetchContent cache,
# or fall back to FetchContent download.
# -------------------------------------------------------
set(GLM_VENDORED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../../build/src/glm/src/glm")
if(EXISTS "${GLM_VENDORED_PATH}/glm/glm.hpp")
    set(GLM_INCLUDE_DIR "${GLM_VENDORED_PATH}")
    message(STATUS "Using vendored GLM: ${GLM_INCLUDE_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        0.9.9.8
    )
    FetchContent_MakeAvailable(glm)
    set(GLM_INCLUDE_DIR "${glm_SOURCE_DIR}")
    message(STATUS "Downloaded GLM: ${GLM_INCLUDE_DIR}")
endif()

# -------------------------------------------------------
# Google Test via FetchContent
# -------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent overriding parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# -------------------------------------------------------
# Test executable
# -------------------------------------------------------
add_executable(vdp1_compute_math_test
    ../Vdp1ComputeMath.cpp
    vdp1_compute_math_test.cpp
)

target_include_directories(vdp1_compute_math_test PRIVATE
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # for Vdp1ComputeCommands.h, Vdp1ComputeMath.h
)

target_link_libraries(vdp1_compute_math_test
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(vdp1_compute_math_test)

# -------------------------------------------------------
# vdp1_compute_dispatch_logic_test
# -------------------------------------------------------
add_executable(vdp1_compute_dispatch_logic_test
    vdp1_compute_dispatch_logic_test.cpp
    ../Vdp1ComputeMath.cpp
)

target_include_directories(vdp1_compute_dispatch_logic_test PRIVATE
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(vdp1_compute_dispatch_logic_test
    GTest::gtest_main
)

gtest_discover_tests(vdp1_compute_dispatch_logic_test)

# -------------------------------------------------------
# debug_snapshot_test
# -------------------------------------------------------
add_executable(debug_snapshot_test
    debug_snapshot_test.cpp
    ../debug/DebugSnapshot.cpp
    ../Vdp1ComputeMath.cpp
)

target_compile_definitions(debug_snapshot_test PRIVATE
    HAVE_STDINT_H=1
)

target_include_directories(debug_snapshot_test PRIVATE
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../   # for vdp1.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(debug_snapshot_test
    GTest::gtest_main
)

gtest_discover_tests(debug_snapshot_test)

# -------------------------------------------------------
# vdp1_jsonl_exporter_test
# -------------------------------------------------------
add_executable(vdp1_jsonl_exporter_test
    vdp1_jsonl_exporter_test.cpp
    ../debug/Vdp1JsonlExporter.cpp
    ../debug/DebugSnapshot.cpp
    ../Vdp1ComputeMath.cpp
)

target_include_directories(vdp1_jsonl_exporter_test PRIVATE
    ${GLM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../
    ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(vdp1_jsonl_exporter_test
    GTest::gtest_main
)

gtest_discover_tests(vdp1_jsonl_exporter_test)

# -------------------------------------------------------
# memory_decode_test
# -------------------------------------------------------
add_executable(memory_decode_test
    memory_decode_test.cpp
    ../debug/MemoryDecode.cpp
)

target_include_directories(memory_decode_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../debug
)

target_link_libraries(memory_decode_test
    GTest::gtest_main
)

gtest_discover_tests(memory_decode_test)

# -------------------------------------------------------
# vdp2_snapshot_test
# -------------------------------------------------------
add_executable(vdp2_snapshot_test
    vdp2_snapshot_test.cpp
    ../debug/Vdp2Snapshot.cpp
)

target_compile_definitions(vdp2_snapshot_test PRIVATE
    HAVE_STDINT_H=1
)

target_include_directories(vdp2_snapshot_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../   # for vdp2.h, vidshared.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(vdp2_snapshot_test
    GTest::gtest_main
)

gtest_discover_tests(vdp2_snapshot_test)

# -------------------------------------------------------
# vdp2_compositor_test (issue #22 T-002 harness)
#
# Standalone host-port test. Links only googletest; the VDP2 reference oracle
# (vdp2_color_oracle.h, T-001) is header-only and lives in this directory, so
# no emulator/vulkan port sources are compiled in. T-005/T-006 add their cases
# to this same .cpp (or new .cpp files registered the same way below).
# -------------------------------------------------------
add_executable(vdp2_compositor_test
    vdp2_compositor_test.cpp
)

target_include_directories(vdp2_compositor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}      # for vdp2_color_oracle.h
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # for Vdp2ColorCalcState.h (T-005)
)

target_link_libraries(vdp2_compositor_test
    GTest::gtest_main
)

gtest_discover_tests(vdp2_compositor_test)

# -------------------------------------------------------
# vdp2_rotbank_test
# -------------------------------------------------------
add_executable(vdp2_rotbank_test
    vdp2_rotbank_test.cpp
)

target_compile_definitions(vdp2_rotbank_test PRIVATE
    HAVE_STDINT_H=1
    HAVE_C99_VARIADIC_MACROS=1
)

target_include_directories(vdp2_rotbank_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../   # for vdp2_rotbank.h, core.h
)

target_link_libraries(vdp2_rotbank_test
    GTest::gtest_main
)

gtest_discover_tests(vdp2_rotbank_test)
