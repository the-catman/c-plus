cmake_minimum_required(VERSION 3.20)

# The compiler is built with clang by default (see PLAN.md). Override with
# -DCMAKE_CXX_COMPILER=... if needed.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  set(CMAKE_CXX_COMPILER clang++)
endif()

project(cplus VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(LLVM 18.1 REQUIRED CONFIG)  # LLVM matches on major.minor
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

# Runtime linked into every C+ program.
add_library(cplusrt STATIC runtime/runtime.c)
set_target_properties(cplusrt PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(cplusrt PRIVATE -Wall -Wextra -O2)

add_executable(cplus
  compiler/main.cpp
  compiler/Diagnostics.cpp
  compiler/Lexer.cpp
  compiler/Parser.cpp
  compiler/Sema.cpp
  compiler/Codegen.cpp)
target_include_directories(cplus SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(cplus PRIVATE ${LLVM_DEFINITIONS_LIST}
  CPLUS_VERSION="${PROJECT_VERSION}"
  CPLUS_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  CPLUS_RUNTIME_LIB="$<TARGET_FILE:cplusrt>"
  CPLUS_LINKER="${CMAKE_C_COMPILER}")
target_compile_options(cplus PRIVATE -Wall -Wextra)
target_link_libraries(cplus PRIVATE LLVM)
add_dependencies(cplus cplusrt)

enable_testing()
add_test(NAME language-tests
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:cplus>
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
