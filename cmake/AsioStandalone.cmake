# 防止同一个 .cmake 文件被 include() 多次时重复执行
include_guard(GLOBAL)

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
      return()
    endif()
  endforeach()

  message(FATAL_ERROR
    "Standalone Asio not found.\n"
    "Expected: ${PROJECT_SOURCE_DIR}/third_party/asio/asio/asio/include/asio.hpp\n"
    "Or specify: -DSECS_ASIO_ROOT=/path/to/asio/include (directory containing asio.hpp)\n"
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
endfunction()

function(secs_link_standalone_asio target)
  secs_ensure_asio_target()
  target_link_libraries(${target} PUBLIC secs_asio)
endfunction()
