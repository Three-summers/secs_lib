cmake_minimum_required(VERSION 3.20)

# SECS-II 编解码简单示例
add_executable(secs2_simple secs2_simple.cpp)
target_link_libraries(secs2_simple PRIVATE secs::ii secs::core)

# HSMS 服务器示例
add_executable(hsms_server hsms_server.cpp)
target_link_libraries(hsms_server PRIVATE secs::hsms secs::ii secs::core)

# HSMS 客户端示例
add_executable(hsms_client hsms_client.cpp)
target_link_libraries(hsms_client PRIVATE secs::hsms secs::ii secs::core)

# TypedHandler 示例
add_executable(typed_handler_example typed_handler_example.cpp)
target_link_libraries(typed_handler_example PRIVATE secs::protocol secs::ii secs::core)

# 如果启用了覆盖率，示例也需要链接 gcov
if(SECS_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_libraries(secs2_simple PRIVATE --coverage)
  target_link_libraries(hsms_server PRIVATE --coverage)
  target_link_libraries(hsms_client PRIVATE --coverage)
  target_link_libraries(typed_handler_example PRIVATE --coverage)
endif()

set_target_properties(secs2_simple hsms_server hsms_client typed_handler_example PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_custom_target(examples DEPENDS secs2_simple hsms_server hsms_client typed_handler_example)
