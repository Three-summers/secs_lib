cmake_minimum_required(VERSION 3.20)

project(my_project LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_SECS_SDK "启用 SECS/HSMS（通过 SDK 内置的 secs_lib C API）" ON)

# 若在 secs-sdk Docker 镜像内构建，会提供 SECS_SDK_PREFIX（包含 secs 与 secs_sdk 的安装前缀）。
# 这里把它加入 CMAKE_PREFIX_PATH，保证 find_package(secs_sdk) 不需要每个项目额外传参。
if(DEFINED ENV{SECS_SDK_PREFIX} AND NOT "$ENV{SECS_SDK_PREFIX}" STREQUAL "")
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{SECS_SDK_PREFIX}")
endif()

# 自动递归收集源文件（避免每新增文件都要手工维护列表）。
# 说明：GLOB_RECURSE 在新增/删除文件时需要重新运行一次 CMake 配置（CONFIGURE_DEPENDS 可自动触发）。
set(PROJECT_SOURCES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.c")
  list(APPEND PROJECT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.c")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
  list(APPEND PROJECT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
endif()
file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(APPEND PROJECT_SOURCES ${_srcs})
list(REMOVE_DUPLICATES PROJECT_SOURCES)

add_executable(run ${PROJECT_SOURCES})

target_include_directories(run PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

find_package(Threads REQUIRED)
target_link_libraries(run PRIVATE Threads::Threads m)
if(UNIX AND NOT APPLE)
  target_link_libraries(run PRIVATE rt)
endif()

if(USE_SECS_SDK)
  # SDK 镜像内置：sysroot + secs_lib + glibc_compat（无需每个项目单独指定 SYSROOT/SECS_LIB/glibc_compat.c）
  find_package(secs_sdk CONFIG REQUIRED)
  target_link_libraries(run PRIVATE secs_sdk::c_api)

  # secs_lib 静态库依赖 pthread；为避免 “DSO missing from command line”，显式追加 pthread。
  if(UNIX AND NOT APPLE)
    target_link_libraries(run PRIVATE pthread)
  endif()

  # 关键点：当目标程序源码全是 C 时，链接到 C++ 静态库仍需使用 C++ 链接器。
  set_property(TARGET run PROPERTY LINKER_LANGUAGE CXX)
endif()

set_target_properties(run PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
