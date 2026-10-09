# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

if(NOT JAM_BUILD_TESTS OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
    OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Jam coverage requires a native Clang test build.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(JAM_GRCOV grcov REQUIRED)
get_filename_component(jam_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(JAM_LLVM_PROFDATA llvm-profdata HINTS "${jam_llvm_bin}" REQUIRED)
find_program(JAM_LLVM_COV llvm-cov HINTS "${jam_llvm_bin}" REQUIRED)
set(jam_coverage_dir "${CMAKE_CURRENT_BINARY_DIR}/coverage")
file(MAKE_DIRECTORY "${jam_coverage_dir}/raw")
# Every provider and importer uses one dependency build. Keep counters private
# to this build; never export instrumentation in Jam's usage requirements.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  add_compile_options(
    "/clang:-fprofile-instr-generate=${jam_coverage_dir}/raw/%m-%p.profraw"
    /clang:-fcoverage-mapping)
else()
  add_compile_options(
    "-fprofile-instr-generate=${jam_coverage_dir}/raw/%m-%p.profraw" -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

function(jam_coverage_collect directory)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type "${target}" TYPE)
    if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      set_property(GLOBAL APPEND PROPERTY JAM_COVERAGE_BINARIES "$<TARGET_FILE:${target}>")
    endif()
    # Assembly probes must retain their original generated code. Reuse module
    # providers, but do not add counters to the codegen consumer itself.
    get_target_property(sources "${target}" SOURCES)
    get_property(explicit TARGET "${target}" PROPERTY JAM_COVERAGE SET)
    get_target_property(coverage "${target}" JAM_COVERAGE)
    if((explicit AND NOT coverage) OR sources MATCHES "(^|[/;])[^/;]*codegen[^/;]*\\.cc($|;)")
      target_compile_options(${target} PRIVATE
        "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:/clang:>-fno-profile-instr-generate"
        "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:/clang:>-fno-coverage-mapping")
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    jam_coverage_collect("${child}")
  endforeach()
endfunction()
function(jam_coverage_configure)
  jam_coverage_collect("${CMAKE_CURRENT_SOURCE_DIR}")
  get_property(binaries GLOBAL PROPERTY JAM_COVERAGE_BINARIES)
  file(GENERATE OUTPUT "${jam_coverage_dir}/binaries-$<CONFIG>.txt"
    CONTENT "$<JOIN:${binaries},\n>\n")
endfunction()
cmake_language(DEFER CALL jam_coverage_configure)

# Reporting consumes the existing CTest run without rebuilding or rerunning it.
add_custom_target(jam_coverage
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/etc/cmake/coverage_report.py"
    --source "${CMAKE_CURRENT_SOURCE_DIR}"
    --coverage "${jam_coverage_dir}" --binaries "${jam_coverage_dir}/binaries-$<CONFIG>.txt"
    --llvm-cov "${JAM_LLVM_COV}" --llvm-profdata "${JAM_LLVM_PROFDATA}"
    --grcov "${JAM_GRCOV}"
  COMMENT "Generating Jam runtime coverage with grcov"
  VERBATIM)
