cmake_minimum_required(VERSION 3.26)

project(_slime C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TODO: Add multi transport mode option
# option(BUILD_RDMA    "Build RDMA"                        ON)
option(BUILD_NVLINK  "Build NVLINK"                     OFF)
option(BUILD_BENCH   "Build transfer engine benchmark"  OFF)
option(BUILD_PYTHON  "Build python wrapper"             OFF)
option(BUILD_TEST    "Build test"                       OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(DLSLIME_INSTALL_PATH "lib" CACHE STRING "Library installation directory")

if (BUILD_NVLINK)
enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)

# Supported NVIDIA architectures.
set(CUDA_SUPPORTED_ARCHS "7.0;7.2;7.5;8.0;8.6;8.7;8.9;9.0;10.0;10.1;12.0")
endif()

include_directories(csrc)
add_subdirectory(csrc)

if (BUILD_BENCH)
add_subdirectory(bench/cpp)
endif (BUILD_BENCH)

if (BUILD_TEST)
add_subdirectory(tests/cpp)
endif (BUILD_TEST)
