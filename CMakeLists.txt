# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(jam-vm
  DESCRIPTION "A jam-backed garbage collector for managed runtimes"
  HOMEPAGE_URL "https://github.com/ekmett/jam-vm"
  LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(JAM_BUILD_TESTS ON CACHE BOOL "Run upstream jam checks")
set(JAM_BUILD_BENCHMARKS ON CACHE BOOL "Build upstream jam benchmark")
enable_testing()
add_subdirectory(upstream/jam jam)
add_library(jam_vm SHARED adapter/jam_vm.cc)
target_sources(jam_vm PRIVATE FILE_SET CXX_MODULES FILES
  adapter/jam.ccm adapter/heap.ccm adapter/weak.ccm)
target_include_directories(jam_vm PUBLIC adapter)
target_link_libraries(jam_vm PRIVATE jam::jam)
set_target_properties(jam_vm PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
  set(JAM_VM_RUNTIME "${CMAKE_SOURCE_DIR}/.toolchains/runtime" CACHE PATH "Matching libc++ and working Darwin unwind runtime")
  target_link_directories(jam_vm PRIVATE "${JAM_VM_RUNTIME}")
  target_link_libraries(jam_vm PRIVATE c++abi)
  set_target_properties(jam_vm PROPERTIES BUILD_RPATH "${JAM_VM_RUNTIME}")
endif()
add_executable(jam-vm-test tests/jam_vm_test.cc)
set_target_properties(jam-vm-test PROPERTIES CXX_STANDARD 14 CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(jam-vm-test PRIVATE jam_vm)
add_test(NAME jam-vm-test COMMAND jam-vm-test)
add_executable(jam-generational-test tests/generational_test.cc)
set_target_properties(jam-generational-test PROPERTIES CXX_STANDARD 14 CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(jam-generational-test PRIVATE jam_vm)
add_test(NAME jam-generational-test COMMAND jam-generational-test)

set(JAM_VM_JAVA_HOME "" CACHE PATH "JDK providing JNI headers for the guest smoke bridge")
if(JAM_VM_JAVA_HOME)
  add_library(jam_jni SHARED tests/jam_jni.c tests/critical_jni.c)
  target_include_directories(jam_jni PRIVATE
    "${JAM_VM_JAVA_HOME}/include"
    "${JAM_VM_JAVA_HOME}/include/$<IF:$<PLATFORM_ID:Darwin>,darwin,linux>"
    "${CMAKE_SOURCE_DIR}/upstream/jdk25/src/hotspot/share/include")
  if(APPLE)
    target_link_options(jam_jni PRIVATE -undefined dynamic_lookup)
  endif()
endif()
