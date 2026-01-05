if(NOT SECS_ENABLE_INTEGRATION_TESTS)
  return()
endif()

find_program(SECS_DOTNET_EXECUTABLE dotnet)

#
# SECS-I（pty）联调：纯 C++，不依赖 dotnet
#
add_executable(integration_secs1_pty secs1/test_secs1_pty.cpp)
target_link_libraries(integration_secs1_pty PRIVATE secs_protocol)

add_test(NAME integration_secs1_pty COMMAND integration_secs1_pty)

add_executable(integration_secs1_pty_required secs1/test_secs1_pty.cpp)
target_link_libraries(integration_secs1_pty_required PRIVATE secs_protocol)
target_compile_definitions(integration_secs1_pty_required PRIVATE SECS_SECS1_REQUIRE_PTY=1)

add_test(NAME integration_secs1_pty_required COMMAND integration_secs1_pty_required)
set_tests_properties(integration_secs1_pty_required PROPERTIES SKIP_RETURN_CODE 77)

#
# HSMS（与 secs4net）联调：
# - 当前沙箱环境禁用 socket()，无法进行真实 TCP 连通性测试；
# - 因此这里用“stdin/stdout 双向管道”承载 HSMS 帧，实现跨语言互通验证
#   （HSMS Select + 数据请求-响应 + SECS-II Item 编解码互通）。
#
add_executable(integration_hsms_pipe_with_secs4net hsms/test_hsms_pipe_with_secs4net.cpp)
target_link_libraries(integration_hsms_pipe_with_secs4net PRIVATE secs_hsms secs_ii secs_core)

add_executable(integration_hsms_tcp_with_secs4net hsms/test_hsms_tcp_with_secs4net.cpp)
target_link_libraries(integration_hsms_tcp_with_secs4net PRIVATE secs_hsms secs_ii secs_core)

add_executable(integration_hsms_tcp_extended_with_secs4net hsms/test_hsms_tcp_extended_with_secs4net.cpp)
target_link_libraries(integration_hsms_tcp_extended_with_secs4net PRIVATE secs_hsms secs_ii secs_core)

if(SECS_DOTNET_EXECUTABLE)
  set(_hsms_peer_proj "${CMAKE_CURRENT_SOURCE_DIR}/hsms/dotnet_peer/HsmsPeer.csproj")
  set(_hsms_peer_dll "${CMAKE_CURRENT_SOURCE_DIR}/hsms/dotnet_peer/bin/Release/net8.0/HsmsPeer.dll")

  add_custom_command(
    OUTPUT "${_hsms_peer_dll}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/.dotnet_cache/dotnet_home"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_http"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_plugins"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_scratch"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/.dotnet_cache/msbuild_user"
    COMMAND "${CMAKE_COMMAND}" -E env
            MSBuildEnableWorkloadResolver=false
            DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
            DOTNET_CLI_TELEMETRY_OPTOUT=1
            DOTNET_CLI_HOME=${CMAKE_SOURCE_DIR}/.dotnet_cache/dotnet_home
            MSBuildUserExtensionsPath=${CMAKE_SOURCE_DIR}/.dotnet_cache/msbuild_user
            NUGET_PACKAGES=/home/say/.nuget/packages
            NUGET_HTTP_CACHE_PATH=${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_http
            NUGET_PLUGINS_CACHE_PATH=${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_plugins
            NUGET_SCRATCH=${CMAKE_SOURCE_DIR}/.dotnet_cache/nuget_scratch
            "${SECS_DOTNET_EXECUTABLE}" build "${_hsms_peer_proj}"
              -c Release
              -v minimal
              -p:NuGetAudit=false
              -p:RestoreIgnoreFailedSources=true
    DEPENDS
      "${_hsms_peer_proj}"
      "${CMAKE_CURRENT_SOURCE_DIR}/hsms/dotnet_peer/Program.cs"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/secs4net/src/Secs4Net/Secs4Net.csproj"
    VERBATIM
  )

  add_custom_target(integration_hsms_peer ALL DEPENDS "${_hsms_peer_dll}")
  add_dependencies(integration_hsms_pipe_with_secs4net integration_hsms_peer)
  add_dependencies(integration_hsms_tcp_with_secs4net integration_hsms_peer)
  add_dependencies(integration_hsms_tcp_extended_with_secs4net integration_hsms_peer)

  target_compile_definitions(integration_hsms_pipe_with_secs4net PRIVATE
    SECS_HSMS_DOTNET_EXECUTABLE="$<SHELL_PATH:${SECS_DOTNET_EXECUTABLE}>"
    SECS_HSMS_PEER_DLL="$<SHELL_PATH:${_hsms_peer_dll}>"
  )
  target_compile_definitions(integration_hsms_tcp_with_secs4net PRIVATE
    SECS_HSMS_DOTNET_EXECUTABLE="$<SHELL_PATH:${SECS_DOTNET_EXECUTABLE}>"
    SECS_HSMS_PEER_DLL="$<SHELL_PATH:${_hsms_peer_dll}>"
  )
  target_compile_definitions(integration_hsms_tcp_extended_with_secs4net PRIVATE
    SECS_HSMS_DOTNET_EXECUTABLE="$<SHELL_PATH:${SECS_DOTNET_EXECUTABLE}>"
    SECS_HSMS_PEER_DLL="$<SHELL_PATH:${_hsms_peer_dll}>"
  )

  add_test(NAME integration_hsms_pipe_with_secs4net COMMAND integration_hsms_pipe_with_secs4net)
  add_test(NAME integration_hsms_tcp_with_secs4net COMMAND integration_hsms_tcp_with_secs4net)
  add_test(NAME integration_hsms_tcp_extended_with_secs4net COMMAND integration_hsms_tcp_extended_with_secs4net)
  set_tests_properties(integration_hsms_tcp_with_secs4net PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(integration_hsms_tcp_extended_with_secs4net PROPERTIES SKIP_RETURN_CODE 77)
endif()
