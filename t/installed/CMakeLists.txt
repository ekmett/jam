# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(jam-consumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(jam CONFIG REQUIRED)
add_executable(jam-consumer main.cc)
target_link_libraries(jam-consumer PRIVATE jam::jam)
enable_testing()
add_test(NAME installed-consumer COMMAND jam-consumer)
