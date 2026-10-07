# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

add_library(jam-vm SHARED adapter/jam_vm.cc)
add_library(jam::vm ALIAS jam-vm)
target_sources(jam-vm PRIVATE FILE_SET CXX_MODULES FILES
  adapter/jam.ccm adapter/heap.ccm adapter/weak.ccm)
target_include_directories(jam-vm PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapter>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(jam-vm PRIVATE JAM_VM_BUILD)
target_link_libraries(jam-vm PRIVATE jam::jam)
set_target_properties(jam-vm PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
  set(JAM_VM_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/.toolchains/runtime" CACHE PATH "Matching libc++ and working Darwin unwind runtime")
  target_link_directories(jam-vm PRIVATE "${JAM_VM_RUNTIME}")
  target_link_libraries(jam-vm PRIVATE c++abi)
  set_target_properties(jam-vm PROPERTIES BUILD_RPATH "${JAM_VM_RUNTIME}")
endif()
if(JAM_BUILD_TESTS)
  add_executable(jam-vm-test tests/jam_vm_test.cc)
  set_target_properties(jam-vm-test PROPERTIES CXX_STANDARD 14 CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(jam-vm-test PRIVATE jam-vm)
  add_test(NAME jam-vm-test COMMAND jam-vm-test)
  add_executable(jam-generational-test tests/generational_test.cc)
  set_target_properties(jam-generational-test PROPERTIES CXX_STANDARD 14 CXX_SCAN_FOR_MODULES OFF)
  target_link_libraries(jam-generational-test PRIVATE jam-vm)
  add_test(NAME jam-generational-test COMMAND jam-generational-test)
  if(WIN32)
    target_link_libraries(jam-vm-test PRIVATE onecore)
    target_link_libraries(jam-generational-test PRIVATE onecore)
  endif()

  set_tests_properties(jam-vm-test jam-generational-test PROPERTIES LABELS vm)
endif()

set(JAM_VM_JAVA_HOME "" CACHE PATH "JDK providing JNI headers for the guest smoke bridge")
set(JAM_VM_HOTSPOT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/jdk25" CACHE PATH "Prepared HotSpot source providing the Jam VM exports")
if(JAM_VM_JAVA_HOME)
  add_library(jam_jni SHARED tests/jam_jni.c tests/critical_jni.c)
  set_target_properties(jam_jni PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  target_include_directories(jam_jni PRIVATE
    "${JAM_VM_JAVA_HOME}/include"
    "${JAM_VM_JAVA_HOME}/include/$<IF:$<PLATFORM_ID:Windows>,win32,$<IF:$<PLATFORM_ID:Darwin>,darwin,linux>>"
    "${JAM_VM_HOTSPOT_SOURCE}/src/hotspot/share/include")
  if(APPLE)
    target_link_options(jam_jni PRIVATE -undefined dynamic_lookup)
  endif()
endif()

# Runtime tools receive this directory explicitly; standalone wrappers default
# to vm/build-jam. Keep DLLs next to the C ABI tests on Windows.
foreach(target jam-vm jam-vm-test jam-generational-test jam_jni)
  if(TARGET ${target})
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_BINARY_DIR}>"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${PROJECT_BINARY_DIR}>"
      ARCHIVE_OUTPUT_DIRECTORY "$<1:${PROJECT_BINARY_DIR}>")
  endif()
endforeach()
configure_file("${jam_native_SOURCE_DIR}/LICENSE.md"
  "${PROJECT_BINARY_DIR}/native-LICENSE.md" COPYONLY)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(JAM_BUILD_TESTS)
  add_test(NAME jam-prepare-graal-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/prepare-graal-test.py")
  set_tests_properties(jam-prepare-graal-test PROPERTIES LABELS vm)
endif()
set(jam_vm_environment "JAM_NATIVE_BUILD=${PROJECT_BINARY_DIR}")
foreach(runtime jdk graalvm)
  add_custom_target(jam-${runtime}
    COMMAND "${CMAKE_COMMAND}" -E env "${jam_vm_environment}"
      "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_runtime.py" ${runtime}
    DEPENDS jam-vm
    USES_TERMINAL VERBATIM)
endforeach()
add_custom_target(jam-substratevm DEPENDS jam-graalvm)

# Runtime checks are explicit: an ordinary ctest needs no downloaded JDK.
set(JAM_VM_TEST_RUNTIMES "" CACHE STRING "Runtime checks to register: jdk;graalvm;substratevm")
if(JAM_BUILD_TESTS)
  foreach(runtime IN LISTS JAM_VM_TEST_RUNTIMES)
    if(NOT runtime MATCHES "^(jdk|graalvm|substratevm)$")
      message(FATAL_ERROR "Unknown JAM_VM_TEST_RUNTIMES entry: ${runtime}")
    endif()
    add_test(NAME jam-${runtime}
      COMMAND "${CMAKE_COMMAND}" -E env "${jam_vm_environment}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_runtime.py" ${runtime})
    set_tests_properties(jam-${runtime} PROPERTIES LABELS "${runtime}" RUN_SERIAL TRUE)
  endforeach()
endif()

set_target_properties(jam-vm PROPERTIES EXPORT_NAME vm)
install(TARGETS jam-vm EXPORT jamTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES adapter/jam_vm.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
