# 防止同一个 .cmake 文件被 include() 多次时重复执行
include_guard(GLOBAL)

# 依赖策略（spdlog，header-only）：
# 1) 优先使用仓库内 vendored spdlog（third_party/spdlog/include）
# 2) 允许用户通过 -DSECS_SPDLOG_ROOT=... 指定外部 spdlog include 目录
# 3) 若以上都不存在，且开启了 SECS_FETCH_SPDLOG，则用 FetchContent 自动拉取 spdlog
#
# 注意：FetchContent 需要网络；在无网络环境可用 -DSECS_FETCH_SPDLOG=OFF 并提供 SECS_SPDLOG_ROOT。
option(SECS_FETCH_SPDLOG "Fetch spdlog automatically when missing" ${SECS_PROJECT_IS_TOP_LEVEL})
set(SECS_FETCH_SPDLOG_GIT_REPOSITORY "https://github.com/gabime/spdlog.git"
  CACHE STRING "Git repository used when SECS_FETCH_SPDLOG is enabled")
set(SECS_FETCH_SPDLOG_GIT_TAG "v1.13.0"
  CACHE STRING "Git tag/commit used when SECS_FETCH_SPDLOG is enabled")

function(secs_fetch_spdlog_include_dir out_var)
  include(FetchContent)

  # 仅声明一次，避免重复下载/配置。
  #
  # 注意：CMake 4.x 开始不再推荐直接调用 FetchContent_Populate()（CMP0169），
  # 这里改用 FetchContent_MakeAvailable() 来完成下载/解压与可用性初始化。
  if(NOT DEFINED secs_spdlog_fc_SOURCE_DIR)
    FetchContent_Declare(secs_spdlog_fc
      GIT_REPOSITORY "${SECS_FETCH_SPDLOG_GIT_REPOSITORY}"
      GIT_TAG "${SECS_FETCH_SPDLOG_GIT_TAG}"
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(secs_spdlog_fc)
  endif()

  # spdlog 仓库的头文件目录为 <repo>/include
  set(${out_var} "${secs_spdlog_fc_SOURCE_DIR}/include" PARENT_SCOPE)
endfunction()

function(secs_resolve_spdlog_include_dir out_var)
  set(candidates
    "${PROJECT_SOURCE_DIR}/third_party/spdlog/include"
  )

  if(DEFINED SECS_SPDLOG_ROOT AND NOT "${SECS_SPDLOG_ROOT}" STREQUAL "")
    list(APPEND candidates
      "${SECS_SPDLOG_ROOT}"
      "${SECS_SPDLOG_ROOT}/include"
    )
  endif()

  foreach(dir IN LISTS candidates)
    if(EXISTS "${dir}/spdlog/spdlog.h")
      set(${out_var} "${dir}" PARENT_SCOPE)
      set(_secs_spdlog_provider "vendored")
      if(DEFINED SECS_SPDLOG_ROOT AND NOT "${SECS_SPDLOG_ROOT}" STREQUAL "")
        string(FIND "${dir}" "${SECS_SPDLOG_ROOT}" _secs_spdlog_root_pos)
        if(_secs_spdlog_root_pos EQUAL 0)
          set(_secs_spdlog_provider "root")
        endif()
      endif()
      set(SECS_SPDLOG_PROVIDER "${_secs_spdlog_provider}" CACHE STRING "How spdlog is provided (vendored/root/system/fetch)" FORCE)
      set(SECS_SPDLOG_INCLUDE_DIR "${dir}" CACHE PATH "spdlog include directory used by secs" FORCE)
      return()
    endif()
  endforeach()

  # 系统路径：例如安装了 libspdlog-dev 或通过包管理器提供了 include 目录
  find_path(_secs_spdlog_system_include_dir
    NAMES spdlog/spdlog.h
  )
  if(_secs_spdlog_system_include_dir)
    set(${out_var} "${_secs_spdlog_system_include_dir}" PARENT_SCOPE)
    set(SECS_SPDLOG_PROVIDER "system" CACHE STRING "How spdlog is provided (vendored/root/system/fetch)" FORCE)
    set(SECS_SPDLOG_INCLUDE_DIR "${_secs_spdlog_system_include_dir}" CACHE PATH "spdlog include directory used by secs" FORCE)
    return()
  endif()

  if(SECS_FETCH_SPDLOG)
    secs_fetch_spdlog_include_dir(fetched_include_dir)
    if(EXISTS "${fetched_include_dir}/spdlog/spdlog.h")
      set(${out_var} "${fetched_include_dir}" PARENT_SCOPE)
      set(SECS_SPDLOG_PROVIDER "fetch" CACHE STRING "How spdlog is provided (vendored/root/system/fetch)" FORCE)
      set(SECS_SPDLOG_INCLUDE_DIR "${fetched_include_dir}" CACHE PATH "spdlog include directory used by secs" FORCE)
      return()
    endif()
  endif()

  message(FATAL_ERROR
    "spdlog not found.\n"
    "Tried: vendored ${PROJECT_SOURCE_DIR}/third_party/spdlog/include/spdlog/spdlog.h\n"
    "Or specify: -DSECS_SPDLOG_ROOT=/path/to/spdlog/include (directory containing spdlog/spdlog.h)\n"
    "Or enable auto-fetch: -DSECS_FETCH_SPDLOG=ON (requires network access)\n"
  )
endfunction()

function(secs_ensure_spdlog_target)
  if(TARGET secs_spdlog)
    return()
  endif()

  secs_resolve_spdlog_include_dir(spdlog_include_dir)

  add_library(secs_spdlog INTERFACE)
  target_include_directories(secs_spdlog SYSTEM INTERFACE
    $<BUILD_INTERFACE:${spdlog_include_dir}>
  )
  target_compile_definitions(secs_spdlog INTERFACE
    SPDLOG_NO_EXCEPTIONS
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
  )
endfunction()

function(secs_link_spdlog target)
  secs_ensure_spdlog_target()
  target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:secs_spdlog>)
endfunction()
