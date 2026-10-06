# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

foreach(name IN ITEMS heap heap-mode root typed scope units simd generation weak finalizer)
  add_executable(${name}-test ${name}-test.cc)
  target_link_libraries(${name}-test PRIVATE jam::jam)
  if(WIN32)
    target_compile_options(${name}-test PRIVATE /clang:-Wall /clang:-Wextra /WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${name}-test PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${name}-test COMMAND ${name}-test)
  set_tests_properties(${name}-test PROPERTIES TIMEOUT 60)
endforeach()

target_sources(heap-test PRIVATE compactor-checks.cc)

target_sources(typed-test PRIVATE work-checks.cc)
