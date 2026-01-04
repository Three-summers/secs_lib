cmake_minimum_required(VERSION 3.20)

project(secs
  VERSION 0.1.0
  LANGUAGES C CXX
)

option(SECS_ENABLE_TESTS "Build unit tests" ON)
option(SECS_ENABLE_COVERAGE "Enable coverage helper targets/flags" OFF)
option(SECS_BUILD_EXAMPLES "Build example programs" ON)
option(SECS_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

# 用于判断是独立构建还是作为依赖被嵌入，PROJECT_SOURCE_DIR 取决于当前作用域最近的一次 project()
set(SECS_PROJECT_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(SECS_PROJECT_IS_TOP_LEVEL ON)
endif()

option(SECS_ENABLE_INSTALL "Enable install() rules and find_package() config" ${SECS_PROJECT_IS_TOP_LEVEL})

# 用于安装的模块
include(GNUInstallDirs)
# 用于生成可被 find_package 找到的配置文件
include(CMakePackageConfigHelpers)

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
set_target_properties(secs_core PROPERTIES EXPORT_NAME core)
target_compile_features(secs_core PUBLIC cxx_std_20)

target_include_directories(secs_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(secs_secs1
  src/secs1/block.cpp
  src/secs1/link.cpp
  src/secs1/state_machine.cpp
  src/secs1/timer.cpp
)
add_library(secs::secs1 ALIAS secs_secs1)
set_target_properties(secs_secs1 PROPERTIES EXPORT_NAME secs1)
target_compile_features(secs_secs1 PUBLIC cxx_std_20)

target_include_directories(secs_secs1
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_secs1 PUBLIC secs_core)

add_library(secs_ii
  src/ii/codec.cpp
  src/ii/item.cpp
)
add_library(secs::ii ALIAS secs_ii)
set_target_properties(secs_ii PROPERTIES EXPORT_NAME ii)
target_compile_features(secs_ii PUBLIC cxx_std_20)

target_include_directories(secs_ii
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_ii PUBLIC secs_core)

add_library(secs_hsms
  src/hsms/message.cpp
  src/hsms/timer.cpp
  src/hsms/connection.cpp
  src/hsms/session.cpp
)
add_library(secs::hsms ALIAS secs_hsms)
set_target_properties(secs_hsms PROPERTIES EXPORT_NAME hsms)
target_compile_features(secs_hsms PUBLIC cxx_std_20)

target_include_directories(secs_hsms
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_hsms PUBLIC secs_core)

add_library(secs_protocol
  src/protocol/system_bytes.cpp
  src/protocol/router.cpp
  src/protocol/session.cpp
)
add_library(secs::protocol ALIAS secs_protocol)
set_target_properties(secs_protocol PROPERTIES EXPORT_NAME protocol)
target_compile_features(secs_protocol PUBLIC cxx_std_20)

target_include_directories(secs_protocol
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_protocol PUBLIC secs_core secs_hsms secs_secs1)

add_library(secs_sml
  src/sml/lexer.cpp
  src/sml/parser.cpp
  src/sml/runtime.cpp
)
add_library(secs::sml ALIAS secs_sml)
set_target_properties(secs_sml PROPERTIES EXPORT_NAME sml)
target_compile_features(secs_sml PUBLIC cxx_std_20)

target_include_directories(secs_sml
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_sml PUBLIC secs_ii secs_core)

add_library(secs_c_api
  src/c_api.cpp
)
add_library(secs::c_api ALIAS secs_c_api)
set_target_properties(secs_c_api PROPERTIES EXPORT_NAME c_api)
target_compile_features(secs_c_api PUBLIC cxx_std_20)

target_include_directories(secs_c_api
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(secs_c_api
  PUBLIC
    secs_protocol
    secs_sml
)

if(MSVC)
  target_compile_options(secs_core PRIVATE /W4 /WX)
  target_compile_options(secs_secs1 PRIVATE /W4 /WX)
  target_compile_options(secs_ii PRIVATE /W4 /WX)
  target_compile_options(secs_hsms PRIVATE /W4 /WX)
  target_compile_options(secs_protocol PRIVATE /W4 /WX)
  target_compile_options(secs_sml PRIVATE /W4 /WX)
  target_compile_options(secs_c_api PRIVATE /W4 /WX)
else()
  target_compile_options(secs_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_secs1 PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_ii PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_hsms PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_sml PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(secs_c_api PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/AsioStandalone.cmake")
secs_link_standalone_asio(secs_core)
add_library(secs::asio ALIAS secs_asio)
set_target_properties(secs_asio PROPERTIES EXPORT_NAME asio)
target_compile_features(secs_asio INTERFACE cxx_std_20)

include(CodeCoverage)

# 覆盖率：只对库添加编译标志（不加链接标志，避免污染示例和用户代码）
if(SECS_ENABLE_COVERAGE)
  secs_enable_coverage_compile(secs_core)
  secs_enable_coverage_compile(secs_secs1)
  secs_enable_coverage_compile(secs_ii)
  secs_enable_coverage_compile(secs_hsms)
  secs_enable_coverage_compile(secs_protocol)
  secs_enable_coverage_compile(secs_sml)
  secs_enable_coverage_compile(secs_c_api)
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

if(SECS_ENABLE_INSTALL)
  install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # 安装 Asio（仅当由本仓库提供：vendored/fetch）。如果用户通过 SECS_ASIO_ROOT 使用外部 Asio，
  # 则不复制其系统头文件，避免把外部环境“打包进” install 前缀。
  if(DEFINED SECS_ASIO_PROVIDER AND (SECS_ASIO_PROVIDER STREQUAL "vendored" OR SECS_ASIO_PROVIDER STREQUAL "fetch"))
    if(NOT DEFINED SECS_ASIO_INCLUDE_DIR OR "${SECS_ASIO_INCLUDE_DIR}" STREQUAL "")
      message(FATAL_ERROR "SECS_ASIO_INCLUDE_DIR is not set; Asio target should have been initialized before install().")
    endif()
    install(FILES "${SECS_ASIO_INCLUDE_DIR}/asio.hpp"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY "${SECS_ASIO_INCLUDE_DIR}/asio"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

  install(TARGETS
      secs_asio
      secs_core
      secs_secs1
      secs_ii
      secs_hsms
      secs_protocol
      secs_sml
      secs_c_api
    EXPORT secsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(EXPORT secsTargets
    FILE secsTargets.cmake
    NAMESPACE secs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secs
  )

  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/secsConfig.cmake.in
    ${PROJECT_BINARY_DIR}/secsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secs
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )

  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/secsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
      ${PROJECT_BINARY_DIR}/secsConfig.cmake
      ${PROJECT_BINARY_DIR}/secsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secs
  )
endif()
