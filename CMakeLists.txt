# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

cmake_minimum_required(VERSION 4.4)
project(jam VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(JAM_BUILD_TESTS "Build heap and tracing checks" ${PROJECT_IS_TOP_LEVEL})
option(JAM_BUILD_BENCHMARKS "Build the compaction benchmark" OFF)
option(JAM_BUILD_VM "Build the managed-runtime adapter and runtime targets" OFF)
option(JAM_ENABLE_COVERAGE "Collect runtime test coverage with Clang and grcov" OFF)
option(JAM_BUILD_DOCS "Build Doxygen documentation" OFF)
option(JAM_CONTEXT_X28 "Reserve AArch64 x28 for the implicit heap instead of TLS" OFF)

if(JAM_BUILD_VM AND JAM_CONTEXT_X28)
  message(FATAL_ERROR "The VM adapter requires TLS: HotSpot and SubstrateVM reserve x28")
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$")
  message(FATAL_ERROR "jam requires macOS, Linux or Windows virtual-memory aliases")
endif()

include(CheckCXXSourceCompiles)
block()
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  check_cxx_source_compiles([=[
    #include <array>
    template<class T> concept additive_t = requires(T x) { x + x; };
    constexpr int identity(int value) noexcept {
      if consteval { return value; } else { return value; }
    }
    constexpr auto values = [] consteval noexcept { return std::array{1, 2}; }();
    static_assert(additive_t<int> && identity(values[1]) == 2);
  ]=] JAM_HAS_REQUIRED_FEATURES)
  if(NOT JAM_HAS_REQUIRED_FEATURES)
    message(FATAL_ERROR "jam requires C++26 concepts, if consteval and consteval lambdas")
  endif()
  check_cxx_source_compiles([=[
    #include <concepts>
    struct value {
      int member;
      template<class Self>
      constexpr decltype(auto) get(this Self & self) noexcept { return (self.member); }
    };
    constexpr value constant{7};
    static_assert(constant.get() == 7);
    static_assert(std::same_as<decltype(constant.get()), int const &>);
    int probe() { value mutable_value{2}; mutable_value.get() = 3; return mutable_value.get(); }
  ]=] JAM_HAS_EXPLICIT_OBJECT_PARAMETERS)
  if(NOT JAM_HAS_EXPLICIT_OBJECT_PARAMETERS)
    message(FATAL_ERROR "jam requires explicit object parameters for const-correct heap accessors")
  endif()
endblock()

# A build targets one platform page size; cross builds provide it explicitly.
set(JAM_PAGE_BYTES "" CACHE STRING "Platform page size in bytes (detected for native builds)")
if(NOT JAM_PAGE_BYTES)
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross builds must set JAM_PAGE_BYTES to the target page size")
  endif()
  try_run(jam_page_run jam_page_compile
    SOURCE_FROM_CONTENT jam-page-size.cc [=[
      #include <cstdio>
      #if defined(_WIN32)
      #include <windows.h>
      #else
      #include <unistd.h>
      #endif
      int main() {
        #if defined(_WIN32)
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        std::printf("%lu", static_cast<unsigned long>(info.dwPageSize));
        #else
        std::printf("%ld", static_cast<long>(sysconf(_SC_PAGESIZE)));
        #endif
      }
    ]=]
    RUN_OUTPUT_VARIABLE jam_page_output)
  if(NOT jam_page_compile OR NOT jam_page_run EQUAL 0)
    message(FATAL_ERROR "Cannot determine the platform page size; set JAM_PAGE_BYTES")
  endif()
  set(JAM_PAGE_BYTES "${jam_page_output}" CACHE STRING "Platform page size in bytes" FORCE)
endif()
if(NOT JAM_PAGE_BYTES MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "JAM_PAGE_BYTES must be a positive integer")
endif()

# Instrument module providers and consumers consistently in this configuration.
# Directory flags do not become installed package usage requirements.
if(JAM_ENABLE_COVERAGE)
  include(etc/cmake/Coverage.cmake)
endif()

include(FetchContent)
# Native owns CPU/OS admission and the source-targeted SIMD implementations.
set(NATIVE_BUILD_TESTS OFF CACHE BOOL "Build native dependency tests")
set(NATIVE_BUILD_DOCS OFF CACHE BOOL "Build native dependency documentation")
set(NATIVE_ENABLE_EXCEPTIONS ON CACHE BOOL "Enable native exceptions" FORCE)
if(NOT TARGET native::native)
  if(NOT FETCHCONTENT_SOURCE_DIR_JAM_NATIVE AND
      NOT FETCHCONTENT_TRY_FIND_PACKAGE_MODE STREQUAL "NEVER")
    find_package(native QUIET CONFIG COMPONENTS native)
    if(TARGET native::headers AND NOT TARGET native::native)
      message(FATAL_ERROR "Jam requires Native's host modules; the installed Native package contains only headers")
    endif()
  endif()
  if(NOT TARGET native::native)
    FetchContent_Declare(jam_native
      GIT_REPOSITORY https://github.com/ekmett/native.git
      GIT_TAG main)
    FetchContent_MakeAvailable(jam_native)
  endif()
endif()
FetchContent_Declare(work
  GIT_REPOSITORY https://github.com/ekmett/work.git
  GIT_TAG main
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(work)

find_package(Threads REQUIRED)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(jam STATIC)
add_library(jam::jam ALIAS jam)
target_compile_features(jam PUBLIC cxx_std_26)
set_target_properties(jam PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
set(jam_interfaces
  src/jam.ccm src/jam_unqualified.ccm
  src/jam/heap.ccm src/jam/mapping.ccm
  src/jam/packed.ccm src/jam/units.ccm src/jam/simd.ccm)
set(jam_implementations
  src/jam/mapping_impl.cc src/jam/packed_impl.cc)
target_sources(jam PUBLIC FILE_SET modules TYPE CXX_MODULES
  BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${jam_interfaces})
target_sources(jam PRIVATE ${jam_implementations})
target_link_libraries(jam PUBLIC Threads::Threads native::native hint::hint work::work)
target_compile_definitions(jam PUBLIC JAM_PAGE_BYTES=${JAM_PAGE_BYTES})
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  block()
    # A Homebrew LLVM's headers can be newer than the SDK's libc++ runtime.
    get_filename_component(jam_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(jam_libcxx "${jam_compiler_bin}/../lib/c++")
    if(EXISTS "${jam_libcxx}/libc++.dylib")
      target_link_directories(jam PUBLIC "$<BUILD_INTERFACE:${jam_libcxx}>")
    endif()
  endblock()
endif()

if(WIN32)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Windows jam builds require clang-cl with the MSVC SDK")
  endif()
  target_link_libraries(jam PRIVATE onecore)
  # Checked ratio arithmetic uses Clang's 128-bit compiler runtime helpers.
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-runtime-dir
    OUTPUT_VARIABLE jam_runtime OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(jam_builtins clang_rt.builtins-aarch64)
  else()
    set(jam_builtins clang_rt.builtins-x86_64)
  endif()
  if(NOT EXISTS "${jam_runtime}/${jam_builtins}.lib")
    message(FATAL_ERROR "jam requires ${jam_builtins}.lib from the Clang toolchain")
  endif()
  target_link_directories(jam PUBLIC "$<BUILD_INTERFACE:${jam_runtime}>")
  target_link_libraries(jam PUBLIC ${jam_builtins})
  target_compile_options(jam PUBLIC -Wno-unused-command-line-argument)
  target_compile_options(jam PRIVATE /clang:-Wall /clang:-Wextra "$<BUILD_INTERFACE:/WX>")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jam PRIVATE -Wall -Wextra "$<BUILD_INTERFACE:-Werror>")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # CMake's installed BMI rule combines --precompile with an unused -c.
    target_compile_options(jam PRIVATE "$<INSTALL_INTERFACE:-Wno-unused-command-line-argument>")
  endif()
else()
  message(FATAL_ERROR "jam requires a compiler supporting C++26 modules")
endif()
if(JAM_CONTEXT_X28)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    message(FATAL_ERROR "JAM_CONTEXT_X28 requires AArch64")
  endif()
  block()
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -ffixed-x28")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_cxx_source_compiles([=[
      register void * active asm("x28");
      __attribute__((pure)) void * current() noexcept { return active; }
      void * bind(void * value) noexcept { auto old = current(); active = value; return old; }
    ]=] JAM_HAS_CONTEXT_X28)
    if(NOT JAM_HAS_CONTEXT_X28)
      message(FATAL_ERROR "JAM_CONTEXT_X28 requires global register variables and -ffixed-x28")
    endif()
  endblock()
  target_compile_options(jam PUBLIC -ffixed-x28)
  target_compile_definitions(jam PUBLIC JAM_CONTEXT_X28=1)
endif()

if(JAM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(t)
endif()
if(JAM_BUILD_VM)
  enable_language(C)
  add_subdirectory(vm)
endif()
if(JAM_BUILD_BENCHMARKS)
  add_executable(heap-bench bench/heap-bench.cc)
  target_link_libraries(heap-bench PRIVATE jam::jam)
endif()
if(JAM_BUILD_DOCS)
  find_package(Doxygen 1.18 REQUIRED COMPONENTS doxygen dot)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(PANDOC_EXECUTABLE pandoc REQUIRED)
  set(DOXYGEN_PROJECT_NAME jam)
  set(DOXYGEN_PROJECT_BRIEF "A compacting generational garbage collector for C++26")
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_GENERATE_XML YES)
  set(DOXYGEN_PROJECT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/favicon.png")

  set(jam_docs_markdown "${CMAKE_CURRENT_BINARY_DIR}/markdown")
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${jam_docs_markdown}/README.md")
  set(DOXYGEN_FILE_PATTERNS "*.ccm" "*.cc" "*.md")
  set(DOXYGEN_EXTENSION_MAPPING "ccm=C++")
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_EXTRACT_PRIVATE NO)
  set(DOXYGEN_EXTRACT_STATIC NO)
  set(DOXYGEN_HIDE_UNDOC_MEMBERS YES)
  set(DOXYGEN_HIDE_UNDOC_CLASSES YES)
  set(DOXYGEN_SEARCH_INCLUDES NO)
  set(DOXYGEN_MACRO_EXPANSION YES)
  set(DOXYGEN_EXPAND_ONLY_PREDEF YES)
  set(DOXYGEN_PREDEFINED
    "JAM_DOXYGEN=1" "hint_const="
    "hint_pure=" "hint_inline=inline" "hint_cold=" "hint_noreturn="
    "hint_lifetimebound=" "hint_diagnose_if(condition,message)=")
  set(DOXYGEN_EXCLUDE_SYMBOLS "jam::detail" "jam::detail::*")
  set(DOXYGEN_MARKDOWN_ID_STYLE GITHUB)
  set(DOXYGEN_STRIP_FROM_PATH "${jam_docs_markdown}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set(DOXYGEN_GENERATE_TREEVIEW YES)
  # Match native's static menus; Doxygen 1.18's dynamic menus link to absent sections.
  set(DOXYGEN_HTML_DYNAMIC_MENUS NO)
  set(DOXYGEN_HTML_COLORSTYLE AUTO_LIGHT)
  set(DOXYGEN_OBFUSCATE_EMAILS NO)
  set(DOXYGEN_SOURCE_BROWSER YES)
  set(DOXYGEN_INLINE_SOURCES NO)
  set(DOXYGEN_HAVE_DOT YES)
  set(DOXYGEN_DOT_IMAGE_FORMAT svg)
  set(DOXYGEN_INTERACTIVE_SVG YES)
  set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
  set(DOXYGEN_WARN_IF_DOC_ERROR YES)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
  set(DOXYGEN_WARN_LOGFILE "${CMAKE_CURRENT_BINARY_DIR}/docs/warnings.log")
  set(DOXYGEN_QUIET YES)
  file(GLOB jam_vm_guides CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/vm/*.md")
  set(jam_docs_pages README.md LICENSE.md
    THIRD_PARTY_NOTICES.md CODE_OF_CONDUCT.md
    docs/README.md docs/tracing.md docs/lifetimes.md docs/generations.md docs/finalizers.md
    ${jam_vm_guides} vm/README.md vm/LICENSE.md vm/NOTICE.md
    docs/simd.md docs/collector.md docs/single-base.md docs/hosting.md docs/building.md)
  # Preserve relative page links in a build-tree copy. Doxygen needs linked
  # badges as inline HTML; Pandoc continues to render the original Markdown.
  set(jam_docs_inputs)
  foreach(page IN LISTS jam_docs_pages)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/${page}")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${page}" docs_markdown)
    if(page STREQUAL "README.md")
      string(REGEX REPLACE "\\[!\\[([^]]*)\\]\\(([^)]+)\\)\\]\\(([^):]+)\\)"
        "<a href=\"https://github.com/ekmett/jam/blob/main/\\3\"><img src=\"\\2\" alt=\"\\1\" /></a>"
        docs_markdown "${docs_markdown}")
      string(REGEX REPLACE "\\[!\\[([^]]*)\\]\\(([^)]+)\\)\\]\\((https?://[^)]+)\\)"
        "<a href=\"\\3\"><img src=\"\\2\" alt=\"\\1\" /></a>"
        docs_markdown "${docs_markdown}")
    endif()
    get_filename_component(page_directory "${jam_docs_markdown}/${page}" DIRECTORY)
    file(MAKE_DIRECTORY "${page_directory}")
    file(WRITE "${jam_docs_markdown}/${page}" "${docs_markdown}")
    list(APPEND jam_docs_inputs "${jam_docs_markdown}/${page}")
  endforeach()
  set(jam_docs_sources ${jam_interfaces} ${jam_implementations})
  list(TRANSFORM jam_docs_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
  doxygen_add_docs(jam-docs ${jam_docs_sources} ${jam_docs_inputs}
    WORKING_DIRECTORY "${jam_docs_markdown}"
    COMMENT "Build jam API documentation")
  # Keep README image paths valid in both GitHub and the generated main page.
  add_custom_command(TARGET jam-docs POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/images"
      "${CMAKE_CURRENT_BINARY_DIR}/docs/html/assets/images"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/badges"
      "${CMAKE_CURRENT_BINARY_DIR}/docs/html/assets/badges"
    VERBATIM)
  # Removed declarations must not leave stale pages in the generated reference.
  add_custom_target(jam-docs-clean
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
      "${CMAKE_CURRENT_BINARY_DIR}/docs/html" "${CMAKE_CURRENT_BINARY_DIR}/docs/xml"
    VERBATIM)
  add_dependencies(jam-docs jam-docs-clean)
  add_custom_command(TARGET jam-docs POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/docs/site/build.py"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/docs/html"
      "${CMAKE_CURRENT_BINARY_DIR}/site" --name jam --pandoc "${PANDOC_EXECUTABLE}"
    VERBATIM)
endif()

install(TARGETS jam EXPORT jamTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  FILE_SET modules DESTINATION "${CMAKE_INSTALL_LIBDIR}/jam/modules")
install(FILES LICENSE.md
  THIRD_PARTY_NOTICES.md DESTINATION "${CMAKE_INSTALL_DATADIR}/jam")
install(EXPORT jamTargets NAMESPACE jam:: CXX_MODULES_DIRECTORY cxx-modules
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jam")
configure_package_config_file(etc/cmake/jamConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/jamConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jam")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/jamConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/jamConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/jamConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jam")
