add_executable(test_core_buffer test_core_buffer.cpp)
target_link_libraries(test_core_buffer PRIVATE secs_core)
add_test(NAME core_buffer COMMAND test_core_buffer)

add_executable(test_core_event test_core_event.cpp)
target_link_libraries(test_core_event PRIVATE secs_core)
add_test(NAME core_event COMMAND test_core_event)

add_executable(test_core_error test_core_error.cpp)
target_link_libraries(test_core_error PRIVATE secs_core)
add_test(NAME core_error COMMAND test_core_error)

add_executable(test_core_log test_core_log.cpp)
target_link_libraries(test_core_log PRIVATE secs_core)
add_test(NAME core_log COMMAND test_core_log)

add_executable(test_metrics_hook test_metrics_hook.cpp)
target_link_libraries(test_metrics_hook PRIVATE secs_core secs_ii secs_hsms secs_sml)
add_test(NAME metrics_hook COMMAND test_metrics_hook)

add_executable(test_secs1_framing test_secs1_framing.cpp)
target_link_libraries(test_secs1_framing PRIVATE secs_secs1)
add_test(NAME secs1_framing COMMAND test_secs1_framing)

add_executable(test_secs1_block_edge_cases test_secs1_block_edge_cases.cpp)
target_link_libraries(test_secs1_block_edge_cases PRIVATE secs_secs1)
add_test(NAME secs1_block_edge_cases COMMAND test_secs1_block_edge_cases)

if(UNIX)
  add_executable(test_secs1_serial_port_link test_secs1_serial_port_link.cpp)
  target_link_libraries(test_secs1_serial_port_link PRIVATE secs_secs1)
  add_test(NAME secs1_serial_port_link COMMAND test_secs1_serial_port_link)

  add_executable(test_secs1_posix_serial_link test_secs1_posix_serial_link.cpp)
  target_link_libraries(test_secs1_posix_serial_link PRIVATE secs_secs1)
  add_test(NAME secs1_posix_serial_link COMMAND test_secs1_posix_serial_link)
endif()

add_executable(test_secs2_codec test_secs2_codec.cpp)
target_link_libraries(test_secs2_codec PRIVATE secs_ii)
add_test(NAME secs2_codec COMMAND test_secs2_codec)

add_executable(test_hsms_transport test_hsms_transport.cpp)
target_link_libraries(test_hsms_transport PRIVATE secs_hsms)
add_test(NAME hsms_transport COMMAND test_hsms_transport)

add_executable(test_hsms_session_lifetime test_hsms_session_lifetime.cpp)
target_link_libraries(test_hsms_session_lifetime PRIVATE secs_hsms)
add_test(NAME hsms_session_lifetime COMMAND test_hsms_session_lifetime)

add_executable(test_hsms_message test_hsms_message.cpp)
target_link_libraries(test_hsms_message PRIVATE secs_hsms)
add_test(NAME hsms_message COMMAND test_hsms_message)

add_executable(test_hsms_codec_fuzz test_hsms_codec_fuzz.cpp)
target_link_libraries(test_hsms_codec_fuzz PRIVATE secs_hsms)
add_test(NAME hsms_codec_fuzz COMMAND test_hsms_codec_fuzz)

add_executable(test_protocol_session test_protocol_session.cpp)
target_link_libraries(test_protocol_session PRIVATE secs_protocol)
add_test(NAME protocol_session COMMAND test_protocol_session)

add_executable(test_protocol_session_lifetime test_protocol_session_lifetime.cpp)
target_link_libraries(test_protocol_session_lifetime PRIVATE secs_protocol)
add_test(NAME protocol_session_lifetime COMMAND test_protocol_session_lifetime)

add_executable(test_typed_handler test_typed_handler.cpp)
target_link_libraries(test_typed_handler PRIVATE secs_protocol secs_ii secs_core)
add_test(NAME typed_handler COMMAND test_typed_handler)

add_executable(test_standard_messages test_standard_messages.cpp)
target_link_libraries(test_standard_messages PRIVATE secs_ii secs_core)
add_test(NAME standard_messages COMMAND test_standard_messages)

add_executable(test_struct_codec test_struct_codec.cpp)
target_link_libraries(test_struct_codec PRIVATE secs_protocol secs_ii secs_core)
add_test(NAME struct_codec COMMAND test_struct_codec)

add_executable(test_sml_parser test_sml_parser.cpp)
target_link_libraries(test_sml_parser PRIVATE secs_sml secs_ii secs_core)
add_test(NAME sml_parser COMMAND test_sml_parser)

add_executable(test_sml_fuzz test_sml_fuzz.cpp)
target_link_libraries(test_sml_fuzz PRIVATE secs_sml)
add_test(NAME sml_fuzz COMMAND test_sml_fuzz)

add_executable(test_sml_runtime test_sml_runtime.cpp)
target_link_libraries(test_sml_runtime PRIVATE secs_sml)
add_test(NAME sml_runtime COMMAND test_sml_runtime)

add_executable(test_utils_dump test_utils_dump.cpp)
target_link_libraries(test_utils_dump PRIVATE secs_utils)
add_test(NAME utils_dump COMMAND test_utils_dump)

add_executable(test_utils_helpers test_utils_helpers.cpp)
target_link_libraries(test_utils_helpers PRIVATE secs_protocol)
add_test(NAME utils_helpers COMMAND test_utils_helpers)

add_executable(test_tools_recording test_tools_recording.cpp)
target_link_libraries(test_tools_recording PRIVATE secs_tools)
add_test(NAME tools_recording COMMAND test_tools_recording)

add_executable(test_tools_sml_check test_tools_sml_check.cpp)
target_link_libraries(test_tools_sml_check PRIVATE secs_tools)
add_test(NAME tools_sml_check COMMAND test_tools_sml_check)

add_executable(test_ceid_layer test_ceid_layer.cpp)
target_link_libraries(test_ceid_layer PRIVATE secs_protocol)
add_test(NAME ceid_layer COMMAND test_ceid_layer)

add_executable(test_c_api_c test_c_api.c)
target_link_libraries(test_c_api_c PRIVATE secs_c_api)
# 该测试源码为 C，但链接必须走 C++ 链接器（底层实现为 C++20）。
set_target_properties(test_c_api_c PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME c_api_c COMMAND test_c_api_c)

if(UNIX AND SECS_BUILD_EXAMPLES)
  # 跨进程 pipe 联调：真实启动 examples/hsms_pipe_{server,client} 并用管道交叉连接
  add_executable(test_hsms_pipe_examples test_hsms_pipe_examples.cpp)
  add_test(NAME hsms_pipe_examples COMMAND test_hsms_pipe_examples)
  set_tests_properties(hsms_pipe_examples PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(TARGET secs-sml-check)
  add_test(NAME sml_check_cli_sample
    COMMAND $<TARGET_FILE:secs-sml-check> --stats ${PROJECT_SOURCE_DIR}/docs/sml_sample/sample.sml
  )
  set_tests_properties(sml_check_cli_sample PROPERTIES
    PASS_REGULAR_EXPRESSION "No syntax errors"
  )
  add_test(NAME sml_check_cli_invalid
    COMMAND $<TARGET_FILE:secs-sml-check> ${CMAKE_CURRENT_SOURCE_DIR}/sml_check_invalid.sml
  )
  set_tests_properties(sml_check_cli_invalid PROPERTIES WILL_FAIL TRUE)

  add_test(NAME sml_check_cli_semantic_undefined_ref
    COMMAND $<TARGET_FILE:secs-sml-check> ${CMAKE_CURRENT_SOURCE_DIR}/sml_check_undefined_ref.sml
  )
  set_tests_properties(sml_check_cli_semantic_undefined_ref PROPERTIES WILL_FAIL TRUE)

  add_test(NAME sml_check_cli_semantic_type_mismatch
    COMMAND $<TARGET_FILE:secs-sml-check> ${CMAKE_CURRENT_SOURCE_DIR}/sml_check_type_mismatch.sml
  )
  set_tests_properties(sml_check_cli_semantic_type_mismatch PROPERTIES WILL_FAIL TRUE)

  add_test(NAME sml_check_cli_json_format
    COMMAND $<TARGET_FILE:secs-sml-check> --format json ${PROJECT_SOURCE_DIR}/docs/sml_sample/sample.sml
  )
  set_tests_properties(sml_check_cli_json_format PROPERTIES
    PASS_REGULAR_EXPRESSION "\"files\""
  )
endif()

if(SECS_ENABLE_COVERAGE)
  secs_enable_coverage(test_core_buffer)
  secs_enable_coverage(test_core_event)
  secs_enable_coverage(test_core_error)
  secs_enable_coverage(test_core_log)
  secs_enable_coverage(test_metrics_hook)
  secs_enable_coverage(test_secs1_framing)
  secs_enable_coverage(test_secs1_block_edge_cases)
  if(TARGET test_secs1_serial_port_link)
    secs_enable_coverage(test_secs1_serial_port_link)
  endif()
  if(TARGET test_secs1_posix_serial_link)
    secs_enable_coverage(test_secs1_posix_serial_link)
  endif()
  secs_enable_coverage(test_secs2_codec)
  secs_enable_coverage(test_hsms_transport)
  secs_enable_coverage(test_hsms_message)
  secs_enable_coverage(test_hsms_codec_fuzz)
  secs_enable_coverage(test_protocol_session)
  secs_enable_coverage(test_protocol_session_lifetime)
  secs_enable_coverage(test_typed_handler)
  secs_enable_coverage(test_standard_messages)
  secs_enable_coverage(test_struct_codec)
  secs_enable_coverage(test_sml_parser)
  secs_enable_coverage(test_sml_fuzz)
  secs_enable_coverage(test_sml_runtime)
  secs_enable_coverage(test_utils_dump)
  secs_enable_coverage(test_utils_helpers)
  secs_enable_coverage(test_tools_recording)
  secs_enable_coverage(test_tools_sml_check)
  secs_enable_coverage(test_ceid_layer)
  secs_enable_coverage(test_c_api_c)
  if(TARGET test_hsms_pipe_examples)
    secs_enable_coverage(test_hsms_pipe_examples)
  endif()
endif()
