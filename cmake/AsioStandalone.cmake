include_guard(GLOBAL)

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

function(secs_ensure_asio_target)
  if(TARGET secs_asio)
    return()
  endif()

  secs_resolve_asio_include_dir(asio_include_dir)

  add_library(secs_asio INTERFACE)
  target_include_directories(secs_asio INTERFACE "${asio_include_dir}")
  target_compile_definitions(secs_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)

  find_package(Threads REQUIRED)
  target_link_libraries(secs_asio INTERFACE Threads::Threads)
endfunction()

function(secs_link_standalone_asio target)
  secs_ensure_asio_target()
  target_link_libraries(${target} PUBLIC secs_asio)
endfunction()
