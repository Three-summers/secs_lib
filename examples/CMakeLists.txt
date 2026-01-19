cmake_minimum_required(VERSION 3.20)

set(_secs_example_targets)

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
endif()

set_target_properties(${_secs_example_targets} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_custom_target(examples DEPENDS ${_secs_example_targets})
