# SECS 库性能基准

add_executable(bench_core_buffer bench_core_buffer.cpp)
target_link_libraries(bench_core_buffer PRIVATE secs::core)
target_include_directories(bench_core_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_secs2_codec bench_secs2_codec.cpp)
target_link_libraries(bench_secs2_codec PRIVATE secs::core secs::ii)
target_include_directories(bench_secs2_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_hsms_message bench_hsms_message.cpp)
target_link_libraries(bench_hsms_message PRIVATE secs::core secs::hsms)
target_include_directories(bench_hsms_message PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_secs1_block bench_secs1_block.cpp)
target_link_libraries(bench_secs1_block PRIVATE secs::core secs::secs1)
target_include_directories(bench_secs1_block PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_sml_runtime bench_sml_runtime.cpp)
target_link_libraries(bench_sml_runtime PRIVATE secs::core secs::sml secs::ii)
target_include_directories(bench_sml_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_tools_recording bench_tools_recording.cpp)
target_link_libraries(bench_tools_recording PRIVATE secs::core secs::tools)
target_include_directories(bench_tools_recording PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_c_api_render_context bench_c_api_render_context.cpp)
target_link_libraries(bench_c_api_render_context PRIVATE secs::core secs::c_api)
target_include_directories(bench_c_api_render_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_protocol_router bench_protocol_router.cpp)
target_link_libraries(bench_protocol_router PRIVATE secs::core secs::protocol)
target_include_directories(bench_protocol_router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_protocol_system_bytes bench_protocol_system_bytes.cpp)
target_link_libraries(bench_protocol_system_bytes PRIVATE secs::core secs::protocol)
target_include_directories(bench_protocol_system_bytes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set(_secs_bench_targets
  bench_core_buffer
  bench_secs2_codec
  bench_hsms_message
  bench_secs1_block
  bench_sml_runtime
  bench_tools_recording
  bench_c_api_render_context
  bench_protocol_router
  bench_protocol_system_bytes
)

# 基准测试：编译警告等级
if(MSVC)
  foreach(_t IN LISTS _secs_bench_targets)
    target_compile_options(${_t} PRIVATE /W4)
  endforeach()
else()
  foreach(_t IN LISTS _secs_bench_targets)
    target_compile_options(${_t} PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()
endif()

# 可选：Release 构建追加优化编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(MSVC)
    foreach(_t IN LISTS _secs_bench_targets)
      target_compile_options(${_t} PRIVATE /O2)
    endforeach()
  else()
    foreach(_t IN LISTS _secs_bench_targets)
      target_compile_options(${_t} PRIVATE -O3)
    endforeach()
  endif()
endif()

add_custom_target(benchmarks DEPENDS ${_secs_bench_targets})
