# 防止同一个 .cmake 文件被 include() 多次时重复执行
include_guard(GLOBAL)

# 依赖策略：
# 1) 优先使用仓库内 vendored Asio（third_party/asio/...）
# 2) 允许用户通过 -DSECS_ASIO_ROOT=... 指定外部 Asio include 目录
# 3) 若以上都不存在，且开启了 SECS_FETCH_ASIO，则用 FetchContent 自动拉取 standalone Asio
#
# 注意：FetchContent 需要网络；在无网络环境可用 -DSECS_FETCH_ASIO=OFF 并提供 SECS_ASIO_ROOT。
option(SECS_FETCH_ASIO "Fetch standalone Asio automatically when missing" ${SECS_PROJECT_IS_TOP_LEVEL})
set(SECS_FETCH_ASIO_GIT_REPOSITORY "https://github.com/chriskohlhoff/asio.git"
  CACHE STRING "Git repository used when SECS_FETCH_ASIO is enabled")
set(SECS_FETCH_ASIO_GIT_TAG "asio-1-30-2"
  CACHE STRING "Git tag/commit used when SECS_FETCH_ASIO is enabled")

function(secs_fetch_asio_include_dir out_var)
  include(FetchContent)

  # 仅声明一次，避免重复下载/配置
  if(NOT DEFINED secs_asio_fc_SOURCE_DIR)
    FetchContent_Declare(secs_asio_fc
      GIT_REPOSITORY "${SECS_FETCH_ASIO_GIT_REPOSITORY}"
      GIT_TAG "${SECS_FETCH_ASIO_GIT_TAG}"
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(secs_asio_fc)
  endif()

  # standalone Asio 仓库的头文件目录为 <repo>/asio/include
  set(${out_var} "${secs_asio_fc_SOURCE_DIR}/asio/include" PARENT_SCOPE)
endfunction()

# 解析 Asio 头文件目录，有候选项和传入项，根据判断文件来决定选择哪个
function(secs_resolve_asio_include_dir out_var)
  set(candidates
    "${PROJECT_SOURCE_DIR}/third_party/asio/asio/asio/include"
  )

  if(DEFINED SECS_ASIO_ROOT AND NOT "${SECS_ASIO_ROOT}" STREQUAL "")
    list(APPEND candidates
      "${SECS_ASIO_ROOT}"
      "${SECS_ASIO_ROOT}/include"
      "${SECS_ASIO_ROOT}/asio/include"
    )
  endif()

  foreach(dir IN LISTS candidates)
    if(EXISTS "${dir}/asio.hpp" AND EXISTS "${dir}/asio/awaitable.hpp")
      set(${out_var} "${dir}" PARENT_SCOPE)
      set(_secs_asio_provider "vendored")
      if(DEFINED SECS_ASIO_ROOT AND NOT "${SECS_ASIO_ROOT}" STREQUAL "")
        string(FIND "${dir}" "${SECS_ASIO_ROOT}" _secs_asio_root_pos)
        if(_secs_asio_root_pos EQUAL 0)
          set(_secs_asio_provider "root")
        endif()
      endif()
      set(SECS_ASIO_PROVIDER "${_secs_asio_provider}" CACHE STRING "How standalone Asio is provided (vendored/root/fetch)" FORCE)
      set(SECS_ASIO_INCLUDE_DIR "${dir}" CACHE PATH "Standalone Asio include directory used by secs" FORCE)
      return()
    endif()
  endforeach()

  # 如果用户指定了 SECS_ASIO_ROOT，则认为是外部提供（即使路径不合法也在后面报错）
  if(DEFINED SECS_ASIO_ROOT AND NOT "${SECS_ASIO_ROOT}" STREQUAL "")
    set(SECS_ASIO_PROVIDER "root" CACHE STRING "How standalone Asio is provided (vendored/root/fetch)" FORCE)
  endif()

  if(SECS_FETCH_ASIO)
    secs_fetch_asio_include_dir(fetched_include_dir)
    if(EXISTS "${fetched_include_dir}/asio.hpp" AND EXISTS "${fetched_include_dir}/asio/awaitable.hpp")
      set(${out_var} "${fetched_include_dir}" PARENT_SCOPE)
      set(SECS_ASIO_PROVIDER "fetch" CACHE STRING "How standalone Asio is provided (vendored/root/fetch)" FORCE)
      set(SECS_ASIO_INCLUDE_DIR "${fetched_include_dir}" CACHE PATH "Standalone Asio include directory used by secs" FORCE)
      return()
    endif()
  endif()

  message(FATAL_ERROR
    "Standalone Asio not found.\n"
    "Tried: vendored ${PROJECT_SOURCE_DIR}/third_party/asio/asio/asio/include/asio.hpp\n"
    "Or specify: -DSECS_ASIO_ROOT=/path/to/asio/include (directory containing asio.hpp)\n"
    "Or enable auto-fetch: -DSECS_FETCH_ASIO=ON (requires network access)\n"
  )
endfunction()

# 确保 secs_asio 这个 target 存在
function(secs_ensure_asio_target)
  # 不重复创建 target
  if(TARGET secs_asio)
    return()
  endif()

  secs_resolve_asio_include_dir(asio_include_dir)

  add_library(secs_asio INTERFACE)
  target_include_directories(secs_asio INTERFACE
    $<BUILD_INTERFACE:${asio_include_dir}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  # ASIO_STANDALONE 表示不依赖 Boost.Asio，ASIO_NO_DEPRECATED 表示禁用遗弃的接口
  target_compile_definitions(secs_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)

  find_package(Threads REQUIRED)
  target_link_libraries(secs_asio INTERFACE Threads::Threads)

  # Windows（尤其是 MinGW）需要显式链接 Winsock 相关库；MSVC 的 #pragma comment(lib, ...)
  # 在 MinGW 下不会生效，否则会出现 WSAStartup/WSASend/AcceptEx 等符号未解析。
  if(WIN32)
    target_link_libraries(secs_asio INTERFACE ws2_32 mswsock)
  endif()
endfunction()

function(secs_link_standalone_asio target)
  secs_ensure_asio_target()
  target_link_libraries(${target} PUBLIC secs_asio)
endfunction()
