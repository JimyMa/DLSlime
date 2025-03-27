cmake_minimum_required(VERSION 3.10)

project(_slime C CXX CUDA)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(csrc)
add_subdirectory(csrc)
