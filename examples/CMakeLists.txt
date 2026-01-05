cmake_minimum_required(VERSION 3.20)

set(_secs_example_targets)

# SECS-II 编解码简单示例
add_executable(secs2_simple secs2_simple.cpp)
target_link_libraries(secs2_simple PRIVATE secs::ii secs::core)
list(APPEND _secs_example_targets secs2_simple)

# HSMS 服务器示例
add_executable(hsms_server hsms_server.cpp)
target_link_libraries(hsms_server PRIVATE secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_server)

# HSMS 客户端示例
add_executable(hsms_client hsms_client.cpp)
target_link_libraries(hsms_client PRIVATE secs::hsms secs::ii secs::core)
list(APPEND _secs_example_targets hsms_client)

# TypedHandler 示例
add_executable(typed_handler_example typed_handler_example.cpp)
target_link_libraries(typed_handler_example PRIVATE secs::protocol secs::ii secs::core)
list(APPEND _secs_example_targets typed_handler_example)

# SECS-I（MemoryLink）回环示例
add_executable(secs1_loopback secs1_loopback.cpp)
target_link_libraries(secs1_loopback PRIVATE secs::protocol secs::core)
list(APPEND _secs_example_targets secs1_loopback)

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
