cmake_minimum_required(VERSION 3.20)

set(_secs_example_targets)
set(_secs_legacy_example_targets)

# ----------------------------- 精简后的“主示例集合”
# - HSMS：自定义请求-响应 / SMLX
# - SECS-I：自定义请求-响应 / SMLX
# - C API：与 C++ 示例功能对应

add_executable(hsms_custom hsms_custom.cpp)
target_link_libraries(hsms_custom PRIVATE secs::protocol secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_custom)

add_executable(hsms_smlx hsms_smlx.cpp)
target_link_libraries(hsms_smlx PRIVATE secs::protocol secs::hsms secs::sml secs::ii secs::core)
list(APPEND _secs_example_targets hsms_smlx)

add_executable(secs1_custom secs1_custom.cpp)
target_link_libraries(secs1_custom PRIVATE secs::protocol secs::ii secs::core)
list(APPEND _secs_example_targets secs1_custom)

add_executable(secs1_smlx secs1_smlx.cpp)
target_link_libraries(secs1_smlx PRIVATE secs::protocol secs::sml secs::ii secs::core)
list(APPEND _secs_example_targets secs1_smlx)

add_executable(c_api_hsms_custom c_api_hsms_custom.c)
target_link_libraries(c_api_hsms_custom PRIVATE secs::c_api)
set_target_properties(c_api_hsms_custom PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_hsms_custom PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_hsms_custom)

add_executable(c_api_hsms_smlx c_api_hsms_smlx.c)
target_link_libraries(c_api_hsms_smlx PRIVATE secs::c_api)
set_target_properties(c_api_hsms_smlx PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_hsms_smlx PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_hsms_smlx)

add_executable(c_api_secs1_custom c_api_secs1_custom.c)
target_link_libraries(c_api_secs1_custom PRIVATE secs::c_api)
set_target_properties(c_api_secs1_custom PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_secs1_custom PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_secs1_custom)

add_executable(c_api_secs1_smlx c_api_secs1_smlx.c)
target_link_libraries(c_api_secs1_smlx PRIVATE secs::c_api)
set_target_properties(c_api_secs1_smlx PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_secs1_smlx PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_secs1_smlx)

# ----------------------------- Legacy 示例（可选构建，默认关闭）
option(SECS_BUILD_LEGACY_EXAMPLES "Build legacy example programs" OFF)
if(SECS_BUILD_LEGACY_EXAMPLES)
  add_executable(legacy_ceid_dispatcher_tvoc_style legacy/ceid_dispatcher_tvoc_style.cpp)
  target_link_libraries(legacy_ceid_dispatcher_tvoc_style PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_ceid_dispatcher_tvoc_style)

  add_executable(legacy_hsms_client legacy/hsms_client.cpp)
  target_link_libraries(legacy_hsms_client PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_hsms_client)

  add_executable(legacy_hsms_server legacy/hsms_server.cpp)
  target_link_libraries(legacy_hsms_server PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_hsms_server)

  add_executable(legacy_hsms_frame_builder_visual legacy/hsms_frame_builder_visual.cpp)
  target_link_libraries(legacy_hsms_frame_builder_visual PRIVATE secs::utils secs::hsms secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_hsms_frame_builder_visual)

  add_executable(legacy_hsms_hex_dump legacy/hsms_hex_dump.cpp)
  target_link_libraries(legacy_hsms_hex_dump PRIVATE secs::utils secs::hsms secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_hsms_hex_dump)

  add_executable(legacy_hsms_sml_peer legacy/hsms_sml_peer.cpp)
  target_link_libraries(legacy_hsms_sml_peer PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_hsms_sml_peer)

  add_executable(legacy_protocol_custom_reply_example legacy/protocol_custom_reply_example.cpp)
  target_link_libraries(legacy_protocol_custom_reply_example PRIVATE secs::protocol secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_protocol_custom_reply_example)

  add_executable(legacy_secs1_loopback legacy/secs1_loopback.cpp)
  target_link_libraries(legacy_secs1_loopback PRIVATE secs::protocol secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_secs1_loopback)

  add_executable(legacy_secs1_serial_client legacy/secs1_serial_client.cpp)
  target_link_libraries(legacy_secs1_serial_client PRIVATE secs::protocol secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_secs1_serial_client)

  add_executable(legacy_secs1_serial_server legacy/secs1_serial_server.cpp)
  target_link_libraries(legacy_secs1_serial_server PRIVATE secs::protocol secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_secs1_serial_server)

  add_executable(legacy_secs1_sml_peer legacy/secs1_sml_peer.cpp)
  target_link_libraries(legacy_secs1_sml_peer PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_secs1_sml_peer)

  add_executable(legacy_secs2_simple legacy/secs2_simple.cpp)
  target_link_libraries(legacy_secs2_simple PRIVATE secs::utils secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_secs2_simple)

  add_executable(legacy_sml_ceid_complete legacy/sml_ceid_complete.cpp)
  target_link_libraries(legacy_sml_ceid_complete PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_sml_ceid_complete)

  add_executable(legacy_smlx_active_send_example legacy/smlx_active_send_example.cpp)
  target_link_libraries(legacy_smlx_active_send_example PRIVATE secs::protocol secs::sml secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_smlx_active_send_example)

  add_executable(legacy_typed_handler_example legacy/typed_handler_example.cpp)
  target_link_libraries(legacy_typed_handler_example PRIVATE secs::protocol secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_typed_handler_example)

  add_executable(legacy_utils_dump_example legacy/utils_dump_example.cpp)
  target_link_libraries(legacy_utils_dump_example PRIVATE secs::utils secs::ii secs::core)
  list(APPEND _secs_legacy_example_targets legacy_utils_dump_example)

  add_executable(legacy_c_api_ceid_tvoc_style legacy/c_api_ceid_tvoc_style.c)
  target_link_libraries(legacy_c_api_ceid_tvoc_style PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_ceid_tvoc_style PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_ceid_tvoc_style PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_ceid_tvoc_style)

  add_executable(legacy_c_api_hsms_client legacy/c_api_hsms_client.c)
  target_link_libraries(legacy_c_api_hsms_client PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_hsms_client PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_hsms_client PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_hsms_client)

  add_executable(legacy_c_api_hsms_server legacy/c_api_hsms_server.c)
  target_link_libraries(legacy_c_api_hsms_server PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_hsms_server PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_hsms_server PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_hsms_server)

  add_executable(legacy_c_api_protocol_client legacy/c_api_protocol_client.c)
  target_link_libraries(legacy_c_api_protocol_client PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_protocol_client PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_protocol_client PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_protocol_client)

  add_executable(legacy_c_api_protocol_server legacy/c_api_protocol_server.c)
  target_link_libraries(legacy_c_api_protocol_server PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_protocol_server PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_protocol_server PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_protocol_server)

  add_executable(legacy_c_api_protocol_loopback legacy/c_api_protocol_loopback.c)
  target_link_libraries(legacy_c_api_protocol_loopback PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_protocol_loopback PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_protocol_loopback PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_protocol_loopback)

  add_executable(legacy_c_api_secs2_simple legacy/c_api_secs2_simple.c)
  target_link_libraries(legacy_c_api_secs2_simple PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_secs2_simple PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_secs2_simple PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_secs2_simple)

  add_executable(legacy_c_api_sml_ceid_complete legacy/c_api_sml_ceid_complete.c)
  target_link_libraries(legacy_c_api_sml_ceid_complete PRIVATE secs::c_api)
  set_target_properties(legacy_c_api_sml_ceid_complete PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(legacy_c_api_sml_ceid_complete PRIVATE c_std_11)
  list(APPEND _secs_legacy_example_targets legacy_c_api_sml_ceid_complete)

  set_target_properties(${_secs_legacy_example_targets} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
  )
endif()

# SMLX 示例依赖同一份模板文件：复制到 build/examples 目录，便于直接运行
foreach(_t IN ITEMS hsms_smlx secs1_smlx c_api_hsms_smlx c_api_secs1_smlx)
  add_custom_command(TARGET ${_t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/ceid_demo.sml
      $<TARGET_FILE_DIR:${_t}>/ceid_demo.sml
    COMMENT "Copying ceid_demo.sml to build directory"
  )
endforeach()

# HSMS（pipe）示例：通过 stdin/stdout 承载 HSMS 帧（仅在 UNIX 可用，依赖 asio::posix）
if(UNIX)
  add_executable(hsms_pipe_server hsms_pipe_server.cpp)
  target_link_libraries(hsms_pipe_server PRIVATE secs::utils secs::hsms secs::ii secs::core)
  list(APPEND _secs_example_targets hsms_pipe_server)

  add_executable(hsms_pipe_client hsms_pipe_client.cpp)
  target_link_libraries(hsms_pipe_client PRIVATE secs::utils secs::hsms secs::ii secs::core)
  list(APPEND _secs_example_targets hsms_pipe_client)
endif()

# 如果启用了覆盖率，示例也需要链接 gcov
if(SECS_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_t IN LISTS _secs_example_targets)
    target_link_libraries(${_t} PRIVATE --coverage)
  endforeach()
  foreach(_t IN LISTS _secs_legacy_example_targets)
    target_link_libraries(${_t} PRIVATE --coverage)
  endforeach()
endif()

set_target_properties(${_secs_example_targets} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_custom_target(examples DEPENDS ${_secs_example_targets} ${_secs_legacy_example_targets})
