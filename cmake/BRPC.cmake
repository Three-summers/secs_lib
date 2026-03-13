# 防止同一个 .cmake 文件被 include() 多次时重复执行
include_guard(GLOBAL)

# 依赖策略（brpc）：
# 1) 优先使用仓库内 vendored brpc 源码（third_party/brpc）
# 2) 允许用户通过 -DSECS_BRPC_ROOT=... 指定 brpc 源码根目录，或带 pkgconfig 的安装前缀
# 3) 若以上都不存在，且开启了 SECS_FETCH_BRPC，则自动拉取 brpc 源码再参与构建
#
# 注意：
# - brpc 自身仍依赖 Protobuf、gflags、leveldb、OpenSSL 等系统依赖；
# - 自动拉取只负责获取 brpc 源码，不会替你安装这些系统库。
option(SECS_FETCH_BRPC "Fetch brpc source automatically when missing" OFF)
set(SECS_FETCH_BRPC_GIT_REPOSITORY "https://github.com/apache/brpc.git"
  CACHE STRING "Git repository used when SECS_FETCH_BRPC is enabled")
set(SECS_FETCH_BRPC_GIT_TAG "1.16.0"
  CACHE STRING "Git tag/commit used when SECS_FETCH_BRPC is enabled")

function(secs_fetch_brpc_source_dir out_var)
  include(FetchContent)

  FetchContent_GetProperties(secs_brpc_fc)
  if(NOT secs_brpc_fc_POPULATED)
    FetchContent_Declare(secs_brpc_fc
      GIT_REPOSITORY "${SECS_FETCH_BRPC_GIT_REPOSITORY}"
      GIT_TAG "${SECS_FETCH_BRPC_GIT_TAG}"
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_Populate(secs_brpc_fc)
  endif()

  set(${out_var} "${secs_brpc_fc_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(secs_try_build_brpc_from_source source_dir out_target out_provider)
  if(NOT EXISTS "${source_dir}/CMakeLists.txt" OR NOT EXISTS "${source_dir}/src/CMakeLists.txt")
    set(${out_target} "" PARENT_SCOPE)
    set(${out_provider} "" PARENT_SCOPE)
    return()
  endif()

  if(NOT TARGET brpc-static AND NOT TARGET brpc-shared)
    # 关闭 brpc 自带的单测和工具，避免把其工程设置扩散到本仓库默认构建路径。
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "Disable brpc unit tests when building as secs dependency" FORCE)
    set(BUILD_BRPC_TOOLS OFF CACHE BOOL "Disable brpc tools when building as secs dependency" FORCE)
    set(DOWNLOAD_GTEST OFF CACHE BOOL "Disable brpc gtest download when building as secs dependency" FORCE)
    set(WITH_GLOG OFF CACHE BOOL "Build brpc without glog when used by secs" FORCE)

    add_subdirectory("${source_dir}" "${PROJECT_BINARY_DIR}/_deps/brpc" EXCLUDE_FROM_ALL)
  endif()

  if(TARGET brpc-static)
    set(${out_target} "brpc-static" PARENT_SCOPE)
    set(${out_provider} "source" PARENT_SCOPE)
    return()
  endif()

  if(TARGET brpc-shared)
    set(${out_target} "brpc-shared" PARENT_SCOPE)
    set(${out_provider} "source" PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR
    "brpc source tree was added, but neither brpc-static nor brpc-shared target was created.\n"
    "Source dir: ${source_dir}\n"
  )
endfunction()

function(secs_try_find_brpc_with_pkg_config out_target out_provider)
  find_package(PkgConfig QUIET)
  if(NOT PkgConfig_FOUND)
    set(${out_target} "" PARENT_SCOPE)
    set(${out_provider} "" PARENT_SCOPE)
    return()
  endif()

  set(_old_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
  if(DEFINED SECS_BRPC_ROOT AND NOT "${SECS_BRPC_ROOT}" STREQUAL "")
    set(_extra_pkg_config_paths "")
    foreach(_dir IN ITEMS lib lib64 share)
      if(EXISTS "${SECS_BRPC_ROOT}/${_dir}/pkgconfig")
        list(APPEND _extra_pkg_config_paths "${SECS_BRPC_ROOT}/${_dir}/pkgconfig")
      endif()
    endforeach()

    if(_extra_pkg_config_paths)
      list(JOIN _extra_pkg_config_paths ":" _extra_pkg_config_path_joined)
      if("${_old_pkg_config_path}" STREQUAL "")
        set(ENV{PKG_CONFIG_PATH} "${_extra_pkg_config_path_joined}")
      else()
        set(ENV{PKG_CONFIG_PATH} "${_extra_pkg_config_path_joined}:${_old_pkg_config_path}")
      endif()
    endif()
  endif()

  pkg_check_modules(SECS_BRPC_PKG QUIET IMPORTED_TARGET GLOBAL brpc)
  set(ENV{PKG_CONFIG_PATH} "${_old_pkg_config_path}")

  if(TARGET PkgConfig::SECS_BRPC_PKG)
    set(${out_target} "PkgConfig::SECS_BRPC_PKG" PARENT_SCOPE)
    set(${out_provider} "pkg-config" PARENT_SCOPE)
    return()
  endif()

  set(${out_target} "" PARENT_SCOPE)
  set(${out_provider} "" PARENT_SCOPE)
endfunction()

function(secs_ensure_brpc_target)
  if(TARGET secs_brpc)
    return()
  endif()

  # 如果上层工程已经提供了 brpc target，优先复用。
  if(TARGET brpc-static OR TARGET brpc-shared)
    add_library(secs_brpc INTERFACE)
    if(TARGET brpc-static)
      target_link_libraries(secs_brpc INTERFACE brpc-static)
      set(_secs_brpc_provider "preexisting-target")
      set(_secs_brpc_target "brpc-static")
    else()
      target_link_libraries(secs_brpc INTERFACE brpc-shared)
      set(_secs_brpc_provider "preexisting-target")
      set(_secs_brpc_target "brpc-shared")
    endif()
  else()
    set(_source_candidates
      "${PROJECT_SOURCE_DIR}/third_party/brpc"
    )

    if(DEFINED SECS_BRPC_ROOT AND NOT "${SECS_BRPC_ROOT}" STREQUAL "")
      list(APPEND _source_candidates "${SECS_BRPC_ROOT}")
    endif()

    set(_resolved_brpc_target "")
    set(_resolved_brpc_provider "")

    foreach(_candidate IN LISTS _source_candidates)
      secs_try_build_brpc_from_source("${_candidate}" _candidate_target _candidate_provider)
      if(NOT "${_candidate_target}" STREQUAL "")
        set(_resolved_brpc_target "${_candidate_target}")
        if("${_candidate}" STREQUAL "${PROJECT_SOURCE_DIR}/third_party/brpc")
          set(_resolved_brpc_provider "vendored")
        else()
          set(_resolved_brpc_provider "root-source")
        endif()
        break()
      endif()
    endforeach()

    if("${_resolved_brpc_target}" STREQUAL "")
      secs_try_find_brpc_with_pkg_config(_candidate_target _candidate_provider)
      if(NOT "${_candidate_target}" STREQUAL "")
        set(_resolved_brpc_target "${_candidate_target}")
        set(_resolved_brpc_provider "${_candidate_provider}")
      endif()
    endif()

    if("${_resolved_brpc_target}" STREQUAL "" AND SECS_FETCH_BRPC)
      secs_fetch_brpc_source_dir(_fetched_brpc_source_dir)
      secs_try_build_brpc_from_source("${_fetched_brpc_source_dir}" _candidate_target _candidate_provider)
      if(NOT "${_candidate_target}" STREQUAL "")
        set(_resolved_brpc_target "${_candidate_target}")
        set(_resolved_brpc_provider "fetch")
      endif()
    endif()

    if("${_resolved_brpc_target}" STREQUAL "")
      message(FATAL_ERROR
        "brpc not found.\n"
        "Tried:\n"
        "  1) vendored source: ${PROJECT_SOURCE_DIR}/third_party/brpc\n"
        "  2) external root/source: -DSECS_BRPC_ROOT=/path/to/brpc-or-prefix\n"
        "  3) pkg-config: brpc\n"
        "  4) auto-fetch: -DSECS_FETCH_BRPC=ON\n\n"
        "Important: brpc also requires system dependencies such as Protobuf, gflags, leveldb, and OpenSSL.\n"
      )
    endif()

    add_library(secs_brpc INTERFACE)
    target_link_libraries(secs_brpc INTERFACE "${_resolved_brpc_target}")
    set(_secs_brpc_provider "${_resolved_brpc_provider}")
    set(_secs_brpc_target "${_resolved_brpc_target}")
  endif()

  target_compile_features(secs_brpc INTERFACE cxx_std_20)

  if(DEFINED _secs_brpc_target AND TARGET "${_secs_brpc_target}")
    set(_secs_brpc_include_dirs "")
    foreach(_prop IN ITEMS INTERFACE_INCLUDE_DIRECTORIES INCLUDE_DIRECTORIES)
      get_target_property(_prop_value "${_secs_brpc_target}" "${_prop}")
      if(_prop_value AND NOT "${_prop_value}" STREQUAL "_prop_value-NOTFOUND")
        list(APPEND _secs_brpc_include_dirs ${_prop_value})
      endif()
    endforeach()
    if(_secs_brpc_include_dirs)
      list(REMOVE_DUPLICATES _secs_brpc_include_dirs)
      target_include_directories(secs_brpc SYSTEM INTERFACE
        ${_secs_brpc_include_dirs}
      )
    endif()

    set(_secs_brpc_compile_defs "")
    foreach(_prop IN ITEMS INTERFACE_COMPILE_DEFINITIONS COMPILE_DEFINITIONS)
      get_target_property(_prop_value "${_secs_brpc_target}" "${_prop}")
      if(_prop_value AND NOT "${_prop_value}" STREQUAL "_prop_value-NOTFOUND")
        list(APPEND _secs_brpc_compile_defs ${_prop_value})
      endif()
    endforeach()
    if(_secs_brpc_compile_defs)
      list(REMOVE_DUPLICATES _secs_brpc_compile_defs)
      target_compile_definitions(secs_brpc INTERFACE
        ${_secs_brpc_compile_defs}
      )
    endif()

    set(_secs_brpc_link_libs "")
    foreach(_prop IN ITEMS INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
      get_target_property(_prop_value "${_secs_brpc_target}" "${_prop}")
      if(_prop_value AND NOT "${_prop_value}" STREQUAL "_prop_value-NOTFOUND")
        list(APPEND _secs_brpc_link_libs ${_prop_value})
      endif()
    endforeach()

    if(TARGET protoc-gen-mcpack)
      get_target_property(_prop_value protoc-gen-mcpack LINK_LIBRARIES)
      if(_prop_value AND NOT "${_prop_value}" STREQUAL "_prop_value-NOTFOUND")
        list(APPEND _secs_brpc_link_libs ${_prop_value})
      endif()
    endif()

    if(_secs_brpc_link_libs)
      list(REMOVE_ITEM _secs_brpc_link_libs "${_secs_brpc_target}")
      list(REMOVE_DUPLICATES _secs_brpc_link_libs)
      target_link_libraries(secs_brpc INTERFACE
        ${_secs_brpc_link_libs}
      )
    endif()
  endif()

  add_library(secs::brpc ALIAS secs_brpc)

  set(SECS_BRPC_PROVIDER "${_secs_brpc_provider}" CACHE STRING "How brpc is provided (vendored/root-source/pkg-config/fetch/preexisting-target)" FORCE)
  set(SECS_BRPC_TARGET "${_secs_brpc_target}" CACHE STRING "Underlying brpc target used by secs" FORCE)
endfunction()

function(secs_link_brpc target)
  secs_ensure_brpc_target()
  target_link_libraries(${target} PUBLIC secs_brpc)
endfunction()
