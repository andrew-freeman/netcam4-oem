cmake_minimum_required(VERSION 3.16)
project(netcam4_oem_rawabi C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_RAWABI_TESTS "Build RAW ABI tests" ON)

add_subdirectory(tools/rawabi)

if(BUILD_RAWABI_TESTS)
  enable_testing()
  add_subdirectory(tests/rawabi)
endif()
