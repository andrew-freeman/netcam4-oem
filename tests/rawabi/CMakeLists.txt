cmake_minimum_required(VERSION 3.16)

add_executable(rawabi_tests rawabi_tests.cpp)
target_link_libraries(rawabi_tests PRIVATE rawabi_common)
add_test(NAME rawabi_tests COMMAND rawabi_tests)
