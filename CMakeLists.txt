cmake_minimum_required(VERSION 3.20)

project(secs
  VERSION 0.1.0
  LANGUAGES CXX
)

option(SECS_ENABLE_TESTS "Build unit tests" ON)
option(SECS_ENABLE_COVERAGE "Enable coverage helper targets/flags" OFF)
option(SECS_BUILD_EXAMPLES "Build example programs" ON)
option(SECS_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/Modules")

add_library(secs_core
  src/core/buffer.cpp
  src/core/event.cpp
  src/core/error.cpp
)
add_library(secs::core ALIAS secs_core)

target_include_directories(secs_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

add_library(secs_secs1
  src/secs1/block.cpp
  src/secs1/link.cpp
  src/secs1/state_machine.cpp
  src/secs1/timer.cpp
)
add_library(secs::secs1 ALIAS secs_secs1)

target_include_directories(secs_secs1
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_link_libraries(secs_secs1 PUBLIC secs_core)

add_library(secs_ii
  src/ii/codec.cpp
  src/ii/item.cpp
)
add_library(secs::ii ALIAS secs_ii)

target_include_directories(secs_ii
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_link_libraries(secs_ii PUBLIC secs_core)

add_library(secs_hsms
  src/hsms/message.cpp
  src/hsms/timer.cpp
  src/hsms/connection.cpp
  src/hsms/session.cpp
)
add_library(secs::hsms ALIAS secs_hsms)

target_include_directories(secs_hsms
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_link_libraries(secs_hsms PUBLIC secs_core)

add_library(secs_protocol
  src/protocol/system_bytes.cpp
  src/protocol/router.cpp
  src/protocol/session.cpp
)
add_library(secs::protocol ALIAS secs_protocol)

target_include_directories(secs_protocol
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_link_libraries(secs_protocol PUBLIC secs_core secs_hsms secs_secs1)

if(MSVC)
  target_compile_options(secs_core PRIVATE /W4 /WX)
  target_compile_options(secs_secs1 PRIVATE /W4 /WX)
  target_compile_options(secs_ii PRIVATE /W4 /WX)
  target_compile_options(secs_hsms PRIVATE /W4 /WX)
  target_compile_options(secs_protocol PRIVATE /W4 /WX)
else()
  target_compile_options(secs_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_secs1 PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_ii PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_hsms PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/AsioStandalone.cmake")
secs_link_standalone_asio(secs_core)

include(CodeCoverage)

# 覆盖率：只对库添加编译标志（不加链接标志，避免污染示例和用户代码）
if(SECS_ENABLE_COVERAGE)
  secs_enable_coverage_compile(secs_core)
  secs_enable_coverage_compile(secs_secs1)
  secs_enable_coverage_compile(secs_ii)
  secs_enable_coverage_compile(secs_hsms)
  secs_enable_coverage_compile(secs_protocol)
endif()

if(SECS_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SECS_ENABLE_TESTS AND SECS_ENABLE_COVERAGE)
  secs_add_coverage_target(coverage)
endif()

if(SECS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SECS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
