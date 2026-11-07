cmake_minimum_required(VERSION 3.16)

# Detect project version from git or fallback
set(DEFAULT_VERSION "2.1.1")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE RAMFS_GIT_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()
endif()
if(NOT RAMFS_GIT_VERSION)
  set(RAMFS_GIT_VERSION "v${DEFAULT_VERSION}")
endif()

project(ramfs
  VERSION 2.1.1
  DESCRIPTION "High-Performance In-Memory Virtual Filesystem (VFS) in C11"
  HOMEPAGE_URL "https://github.com/AubinMahe/ramfs"
  LANGUAGES C
)

# Standard C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_SHARED_LIBS "Build shared libraries (.so / .dll)" ON)
option(RAMFS_BUILD_TESTS "Build test suites" ON)
option(RAMFS_BUILD_BENCHMARK "Build performance benchmark" ON)
option(RAMFS_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RAMFS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(RAMFS_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(RAMFS_INSTALL "Generate installation and export targets" ON)

# Dependencies
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# Common compilation flags
add_library(ramfs_common_options INTERFACE)
target_compile_definitions(ramfs_common_options INTERFACE
  _POSIX_C_SOURCE=200809L
  _DEFAULT_SOURCE
  _XOPEN_SOURCE=700
  RAMFS_VERSION="${RAMFS_GIT_VERSION}"
  $<$<BOOL:${WIN32}>:BUILDING_RAMFS_DLL>
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ramfs_common_options INTERFACE
    -Wall
    -Wextra
    -pedantic
    -Wformat=2
    -Wformat-security
    -Wstrict-overflow
    -Wno-unused-result
  )
endif()

# Sanitizers and coverage
if(RAMFS_ENABLE_ASAN)
  target_compile_options(ramfs_common_options INTERFACE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined
  )
  target_link_options(ramfs_common_options INTERFACE
    -fsanitize=address,undefined
  )
endif()

if(RAMFS_ENABLE_TSAN)
  target_compile_options(ramfs_common_options INTERFACE
    -fsanitize=thread
    -fno-omit-frame-pointer
  )
  target_link_options(ramfs_common_options INTERFACE
    -fsanitize=thread
  )
endif()

if(RAMFS_ENABLE_COVERAGE)
  target_compile_options(ramfs_common_options INTERFACE --coverage)
  target_link_options(ramfs_common_options INTERFACE --coverage)
endif()

# Source files
set(RAMFS_SOURCES
  src/ramfs_core.c
  src/ramfs_vfs.c
  src/ramfs_posix.c
  src/ramfs_stdio.c
  src/ramfs_archive.c
)

set(RAMFS_PUBLIC_HEADERS
  inc/ramfs.h
)

# -----------------------------------------------------------------------------
# Single-Threaded Library: ramfs
# -----------------------------------------------------------------------------
add_library(ramfs ${RAMFS_SOURCES})
add_library(ramfs::ramfs ALIAS ramfs)

target_link_libraries(ramfs
  PRIVATE
    ramfs_common_options
    ZLIB::ZLIB
)

target_include_directories(ramfs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(ramfs PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  PUBLIC_HEADER "${RAMFS_PUBLIC_HEADERS}"
)

# -----------------------------------------------------------------------------
# Multi-Threaded Library: ramfs_mt
# -----------------------------------------------------------------------------
add_library(ramfs_mt ${RAMFS_SOURCES})
add_library(ramfs::ramfs_mt ALIAS ramfs_mt)

target_link_libraries(ramfs_mt
  PRIVATE
    ramfs_common_options
    ZLIB::ZLIB
    Threads::Threads
)

target_compile_definitions(ramfs_mt PUBLIC RAMFS_THREAD_SAFE)

target_include_directories(ramfs_mt
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(ramfs_mt PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  PUBLIC_HEADER "${RAMFS_PUBLIC_HEADERS}"
)

# -----------------------------------------------------------------------------
# Testing & CTest
# -----------------------------------------------------------------------------
if(RAMFS_BUILD_TESTS)
  include(CTest)

  # Ensure directory expected by test suites for disk tests exists
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build_linux")

  # Single-threaded test suite
  add_executable(test_ramfs
    test/main.c
    test/test_macros.c
  )
  target_link_libraries(test_ramfs
    PRIVATE
      ramfs
      ramfs_common_options
  )
  target_include_directories(test_ramfs PRIVATE inc)
  add_test(NAME test_ramfs COMMAND test_ramfs)
  set_tests_properties(test_ramfs PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  # Multi-threaded test suite
  add_executable(test_ramfs_mt
    test/test_mt.c
  )
  target_link_libraries(test_ramfs_mt
    PRIVATE
      ramfs_mt
      ramfs_common_options
      Threads::Threads
  )
  target_include_directories(test_ramfs_mt PRIVATE inc)
  add_test(NAME test_ramfs_mt COMMAND test_ramfs_mt)
  set_tests_properties(test_ramfs_mt PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()

# -----------------------------------------------------------------------------
# Performance Benchmark
# -----------------------------------------------------------------------------
if(RAMFS_BUILD_BENCHMARK)
  add_executable(perf_benchmark
    test/perf_benchmark.c
  )
  target_link_libraries(perf_benchmark
    PRIVATE
      ramfs
      ramfs_common_options
  )
  target_include_directories(perf_benchmark PRIVATE inc)
endif()

# -----------------------------------------------------------------------------
# Installation & CMake Package Configuration
# -----------------------------------------------------------------------------
if(RAMFS_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS ramfs ramfs_mt
    EXPORT ramfsTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(EXPORT ramfsTargets
    NAMESPACE ramfs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ramfs
  )

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ramfsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ramfsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ramfs
  )

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ramfsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ramfsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ramfsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ramfs
  )
endif()
