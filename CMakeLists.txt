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

# What `import http` needs. A compiler built without it says so when a program imports http. Each is a cache setting, so
# -DCPLUS_CURL_LIBRARY=... on the cmake command line picks another libcurl.
find_path(CPLUS_CURL_INCLUDE curl/curl.h)
find_library(CPLUS_CURL_LIBRARY curl)
set(CPLUS_RUNTIME_SOURCES runtime/runtime.c runtime/json.c runtime/reflect.c runtime/thread.c)
# What `import fs;`, `readLine()` and `stoi` need, which a program links only when it uses them, as the runtime is an archive.
list(APPEND CPLUS_RUNTIME_SOURCES runtime/fs.c runtime/input.c runtime/parse.c)
set(CPLUS_CURL_DEFINITION "")
if(CPLUS_CURL_INCLUDE AND CPLUS_CURL_LIBRARY)
  list(APPEND CPLUS_RUNTIME_SOURCES runtime/http.c)
  set(CPLUS_CURL_DEFINITION CPLUS_CURL_LINK="${CPLUS_CURL_LIBRARY}")
else()
  message(STATUS "No libcurl: programs built with this compiler cannot `import http`")
endif()

# What async needs, its event loop. -DCPLUS_UV_LIBRARY=... picks another libuv.
find_path(CPLUS_UV_INCLUDE uv.h)
find_library(CPLUS_UV_LIBRARY uv)
set(CPLUS_UV_DEFINITION "")
if(CPLUS_UV_INCLUDE AND CPLUS_UV_LIBRARY)
  list(APPEND CPLUS_RUNTIME_SOURCES runtime/loop.c)
  set(CPLUS_UV_DEFINITION CPLUS_UV_LINK="${CPLUS_UV_LIBRARY}")
else()
  message(STATUS "No libuv: programs built with this compiler cannot use async")
endif()
# http::fetch needs both.
if(CPLUS_CURL_DEFINITION AND CPLUS_UV_DEFINITION)
  list(APPEND CPLUS_RUNTIME_SOURCES runtime/fetch.c)
endif()

# Runtime linked into every C+ program.
add_library(cplusrt STATIC ${CPLUS_RUNTIME_SOURCES})
set_target_properties(cplusrt PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
# Programs carry the runtime's debug information, which must not name this machine's folders.
set(CPLUS_RUNTIME_PATHS -ffile-prefix-map=${CMAKE_SOURCE_DIR}=/cplus)
target_compile_options(cplusrt PRIVATE -Wall -Wextra -O2 ${CPLUS_RUNTIME_PATHS})
if(CPLUS_CURL_DEFINITION)
  target_include_directories(cplusrt SYSTEM PRIVATE ${CPLUS_CURL_INCLUDE})
endif()
if(CPLUS_UV_DEFINITION)
  target_include_directories(cplusrt SYSTEM PRIVATE ${CPLUS_UV_INCLUDE})
endif()

# The runtime for programs built with --debug, if the C compiler has AddressSanitizer.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
check_c_source_compiles("int main(void) { return 0; }" CPLUS_HAVE_ASAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
set(CPLUS_RUNTIME_LIB_DEBUG "")
if(CPLUS_HAVE_ASAN)
  add_library(cplusrt_debug STATIC ${CPLUS_RUNTIME_SOURCES})
  set_target_properties(cplusrt_debug PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  target_compile_options(cplusrt_debug PRIVATE -Wall -Wextra -O1 -fsanitize=address
    -fno-omit-frame-pointer -fno-optimize-sibling-calls ${CPLUS_RUNTIME_PATHS})
  target_compile_definitions(cplusrt_debug PRIVATE CPLUS_DEBUG_RUNTIME)
  if(CPLUS_CURL_DEFINITION)
    target_include_directories(cplusrt_debug SYSTEM PRIVATE ${CPLUS_CURL_INCLUDE})
  endif()
  if(CPLUS_UV_DEFINITION)
    target_include_directories(cplusrt_debug SYSTEM PRIVATE ${CPLUS_UV_INCLUDE})
  endif()
  # Turns the addresses in a stack trace into files and lines.
  find_program(CPLUS_SYMBOLIZER llvm-symbolizer HINTS ${LLVM_TOOLS_BINARY_DIR})
  if(CPLUS_SYMBOLIZER)
    target_compile_definitions(cplusrt_debug PRIVATE CPLUS_SYMBOLIZER="${CPLUS_SYMBOLIZER}")
  endif()
  set(CPLUS_RUNTIME_LIB_DEBUG "$<TARGET_FILE:cplusrt_debug>")
else()
  message(STATUS "No AddressSanitizer in the C compiler: cplus --debug will report that it is missing")
endif()

# The runtime for programs built with --race, if the C compiler has ThreadSanitizer.
set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
check_c_source_compiles("int main(void) { return 0; }" CPLUS_HAVE_TSAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
set(CPLUS_RUNTIME_LIB_RACE "")
if(CPLUS_HAVE_TSAN)
  set(CPLUS_RACE_STACK "")
  if(NOT APPLE)  # its linker has no --wrap: see compiler/main.cpp
    add_library(cplusrt_race_stack OBJECT runtime/race_stack.c)
    set_target_properties(cplusrt_race_stack PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    target_compile_options(cplusrt_race_stack PRIVATE -Wall -Wextra -O2 ${CPLUS_RUNTIME_PATHS})
    set(CPLUS_RACE_STACK $<TARGET_OBJECTS:cplusrt_race_stack>)
  endif()
  add_library(cplusrt_race STATIC ${CPLUS_RUNTIME_SOURCES} ${CPLUS_RACE_STACK})
  set_target_properties(cplusrt_race PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  target_compile_options(cplusrt_race PRIVATE -Wall -Wextra -O1 -fsanitize=thread
    -fno-omit-frame-pointer -fno-optimize-sibling-calls ${CPLUS_RUNTIME_PATHS})
  target_compile_definitions(cplusrt_race PRIVATE CPLUS_DEBUG_RUNTIME CPLUS_RACE_RUNTIME)
  if(CPLUS_CURL_DEFINITION)
    target_include_directories(cplusrt_race SYSTEM PRIVATE ${CPLUS_CURL_INCLUDE})
  endif()
  if(CPLUS_UV_DEFINITION)
    target_include_directories(cplusrt_race SYSTEM PRIVATE ${CPLUS_UV_INCLUDE})
  endif()
  find_program(CPLUS_RACE_SYMBOLIZER llvm-symbolizer HINTS ${LLVM_TOOLS_BINARY_DIR})
  if(CPLUS_RACE_SYMBOLIZER)
    target_compile_definitions(cplusrt_race PRIVATE CPLUS_SYMBOLIZER="${CPLUS_RACE_SYMBOLIZER}")
  endif()
  set(CPLUS_RUNTIME_LIB_RACE "$<TARGET_FILE:cplusrt_race>")
else()
  message(STATUS "No ThreadSanitizer in the C compiler: cplus --race will report that it is missing")
endif()

# What `import <sqlite3.h>;` reads C headers with. A compiler built without it says so when a program imports a header.
# -DCPLUS_CLANG_LIBRARY=... picks another libclang, which must be of the same LLVM.
find_path(CPLUS_CLANG_INCLUDE clang-c/Index.h HINTS ${LLVM_INCLUDE_DIRS} NO_DEFAULT_PATH)
find_library(CPLUS_CLANG_LIBRARY NAMES clang-${LLVM_VERSION_MAJOR} clang HINTS ${LLVM_LIBRARY_DIRS} NO_DEFAULT_PATH)
if(NOT (CPLUS_CLANG_INCLUDE AND CPLUS_CLANG_LIBRARY))
  message(STATUS "No libclang for LLVM ${LLVM_PACKAGE_VERSION}: programs built with this compiler cannot import C headers")
endif()

# What `cplus explain` shows, built into the compiler.
file(GLOB CPLUS_FEATURE_EXAMPLES CONFIGURE_DEPENDS examples/features/*.cplus)
set(CPLUS_EMBEDDED_DOCS ${CMAKE_BINARY_DIR}/embedded_docs.cpp)
add_custom_command(OUTPUT ${CPLUS_EMBEDDED_DOCS}
  COMMAND ${CMAKE_COMMAND} -DOUT=${CPLUS_EMBEDDED_DOCS} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/EmbedDocs.cmake
  DEPENDS docs/SPEC.md docs/CHEATSHEET.md cmake/EmbedDocs.cmake ${CPLUS_FEATURE_EXAMPLES}
  COMMENT "Building the docs into the compiler")

add_executable(cplus
  compiler/main.cpp
  compiler/Explain.cpp
  ${CPLUS_EMBEDDED_DOCS}
  compiler/Diagnostics.cpp
  compiler/Lexer.cpp
  compiler/Format.cpp
  compiler/Index.cpp
  compiler/Lsp.cpp
  compiler/CHeaders.cpp
  compiler/Parser.cpp
  compiler/Sema.cpp
  compiler/ConstEval.cpp
  compiler/Codegen.cpp)
target_include_directories(cplus SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(cplus PRIVATE compiler)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(cplus PRIVATE ${LLVM_DEFINITIONS_LIST}
  CPLUS_VERSION="${PROJECT_VERSION}"
  CPLUS_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  CPLUS_RUNTIME_LIB="$<TARGET_FILE:cplusrt>"
  CPLUS_RUNTIME_LIB_DEBUG="${CPLUS_RUNTIME_LIB_DEBUG}"
  CPLUS_RUNTIME_LIB_RACE="${CPLUS_RUNTIME_LIB_RACE}"
  CPLUS_LINKER="${CMAKE_C_COMPILER}"
  ${CPLUS_CURL_DEFINITION}
  ${CPLUS_UV_DEFINITION})
target_compile_options(cplus PRIVATE -Wall -Wextra)
target_link_libraries(cplus PRIVATE LLVM)
if(CPLUS_CLANG_INCLUDE AND CPLUS_CLANG_LIBRARY)
  target_include_directories(cplus SYSTEM PRIVATE ${CPLUS_CLANG_INCLUDE})
  target_compile_definitions(cplus PRIVATE CPLUS_HAVE_CLANG)
  target_link_libraries(cplus PRIVATE ${CPLUS_CLANG_LIBRARY})
endif()
add_dependencies(cplus cplusrt)
if(CPLUS_HAVE_ASAN)
  add_dependencies(cplus cplusrt_debug)
endif()
if(CPLUS_HAVE_TSAN)
  add_dependencies(cplus cplusrt_race)
endif()

enable_testing()
add_test(NAME language-tests
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:cplus>
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
