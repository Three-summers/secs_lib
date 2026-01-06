cmake_minimum_required(VERSION 3.20)

set(_secs_example_targets)

# SECS-II 编解码简单示例
add_executable(secs2_simple secs2_simple.cpp)
target_link_libraries(secs2_simple PRIVATE secs::ii secs::core)
list(APPEND _secs_example_targets secs2_simple)

# C API（C ABI）SECS-II 编解码示例（源文件为 C，但链接需使用 C++ 链接器）
add_executable(c_api_secs2_simple c_api_secs2_simple.c)
target_link_libraries(c_api_secs2_simple PRIVATE secs::c_api)
set_target_properties(c_api_secs2_simple PROPERTIES LINKER_LANGUAGE CXX)
list(APPEND _secs_example_targets c_api_secs2_simple)

# C API（C ABI）HSMS（TCP）示例（源文件为 C，但链接需使用 C++ 链接器）
add_executable(c_api_hsms_server c_api_hsms_server.c)
target_link_libraries(c_api_hsms_server PRIVATE secs::c_api)
set_target_properties(c_api_hsms_server PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_hsms_server PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_hsms_server)

add_executable(c_api_hsms_client c_api_hsms_client.c)
target_link_libraries(c_api_hsms_client PRIVATE secs::c_api)
set_target_properties(c_api_hsms_client PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_hsms_client PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_hsms_client)

# C API（C ABI）协议层示例：protocol session + HSMS（TCP）
add_executable(c_api_protocol_server c_api_protocol_server.c)
target_link_libraries(c_api_protocol_server PRIVATE secs::c_api)
set_target_properties(c_api_protocol_server PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_protocol_server PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_protocol_server)

add_executable(c_api_protocol_client c_api_protocol_client.c)
target_link_libraries(c_api_protocol_client PRIVATE secs::c_api)
set_target_properties(c_api_protocol_client PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(c_api_protocol_client PRIVATE c_std_11)
list(APPEND _secs_example_targets c_api_protocol_client)

# C API（C ABI）协议层回环示例：memory duplex + pthread（仅 UNIX）
if(UNIX)
  add_executable(c_api_protocol_loopback c_api_protocol_loopback.c)
  target_link_libraries(c_api_protocol_loopback PRIVATE secs::c_api)
  set_target_properties(c_api_protocol_loopback PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_features(c_api_protocol_loopback PRIVATE c_std_11)
  list(APPEND _secs_example_targets c_api_protocol_loopback)
endif()

# HSMS 服务器示例
add_executable(hsms_server hsms_server.cpp)
target_link_libraries(hsms_server PRIVATE secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_server)

# HSMS 客户端示例
add_executable(hsms_client hsms_client.cpp)
target_link_libraries(hsms_client PRIVATE secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_client)

# HSMS（TCP）SML 对端：主动/被动都加载同一份 SML，并按规则自动回包
add_executable(hsms_sml_peer hsms_sml_peer.cpp)
target_link_libraries(hsms_sml_peer PRIVATE secs::protocol secs::sml secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_sml_peer)

# TypedHandler 示例
add_executable(typed_handler_example typed_handler_example.cpp)
target_link_libraries(typed_handler_example PRIVATE secs::protocol secs::ii secs::core)
list(APPEND _secs_example_targets typed_handler_example)

# 协议层自定义回包示例（default handler + SECS-II 编码）
add_executable(protocol_custom_reply_example protocol_custom_reply_example.cpp)
target_link_libraries(protocol_custom_reply_example PRIVATE secs::protocol secs::ii secs::core)
list(APPEND _secs_example_targets protocol_custom_reply_example)

# SECS-I（MemoryLink）回环示例
add_executable(secs1_loopback secs1_loopback.cpp)
target_link_libraries(secs1_loopback PRIVATE secs::protocol secs::core)
list(APPEND _secs_example_targets secs1_loopback)

# HSMS（pipe）示例：通过 stdin/stdout 承载 HSMS 帧（仅在 UNIX 可用，依赖 asio::posix）
if(UNIX)
  add_executable(hsms_pipe_server hsms_pipe_server.cpp)
  target_link_libraries(hsms_pipe_server PRIVATE secs::hsms secs::ii secs::core)
  list(APPEND _secs_example_targets hsms_pipe_server)

  add_executable(hsms_pipe_client hsms_pipe_client.cpp)
  target_link_libraries(hsms_pipe_client PRIVATE secs::hsms secs::ii secs::core)
  list(APPEND _secs_example_targets hsms_pipe_client)
endif()

# SECS-I（tty/pty）串口示例：仅在 UNIX 可用（依赖 termios / posix fd）
if(UNIX)
  add_executable(secs1_serial_server secs1_serial_server.cpp)
  target_link_libraries(secs1_serial_server PRIVATE secs::protocol secs::core)
  list(APPEND _secs_example_targets secs1_serial_server)

  add_executable(secs1_serial_client secs1_serial_client.cpp)
  target_link_libraries(secs1_serial_client PRIVATE secs::protocol secs::core)
  list(APPEND _secs_example_targets secs1_serial_client)
endif()

# 如果启用了覆盖率，示例也需要链接 gcov
if(SECS_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_t IN LISTS _secs_example_targets)
    target_link_libraries(${_t} PRIVATE --coverage)
  endforeach()
endif()

set_target_properties(${_secs_example_targets} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_custom_target(examples DEPENDS ${_secs_example_targets})
