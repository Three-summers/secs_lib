set(SECS_RPC_PROTO_FILE
  "${CMAKE_CURRENT_SOURCE_DIR}/proto/secs/rpc/v1/secs_rpc.proto")
set(SECS_RPC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SECS_RPC_GENERATED_SOURCE
  "${SECS_RPC_GENERATED_DIR}/secs/rpc/v1/secs_rpc.pb.cc")
set(SECS_RPC_GENERATED_HEADER
  "${SECS_RPC_GENERATED_DIR}/secs/rpc/v1/secs_rpc.pb.h")

if(TARGET protobuf::protoc)
  set(SECS_RPC_PROTOC_COMMAND "$<TARGET_FILE:protobuf::protoc>")
  set(SECS_RPC_PROTOC_DEP protobuf::protoc)
elseif(DEFINED Protobuf_PROTOC_EXECUTABLE AND NOT "${Protobuf_PROTOC_EXECUTABLE}" STREQUAL "")
  set(SECS_RPC_PROTOC_COMMAND "${Protobuf_PROTOC_EXECUTABLE}")
elseif(DEFINED PROTOBUF_PROTOC_EXECUTABLE AND NOT "${PROTOBUF_PROTOC_EXECUTABLE}" STREQUAL "")
  set(SECS_RPC_PROTOC_COMMAND "${PROTOBUF_PROTOC_EXECUTABLE}")
else()
  message(FATAL_ERROR
    "Unable to locate protoc for secs_rpc generation.\n"
    "Expected protobuf::protoc or Protobuf_PROTOC_EXECUTABLE / PROTOBUF_PROTOC_EXECUTABLE.\n"
  )
endif()

add_custom_command(
  OUTPUT
    "${SECS_RPC_GENERATED_SOURCE}"
    "${SECS_RPC_GENERATED_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${SECS_RPC_GENERATED_DIR}"
  COMMAND "${SECS_RPC_PROTOC_COMMAND}"
    "--cpp_out=${SECS_RPC_GENERATED_DIR}"
    "-I" "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    "${SECS_RPC_PROTO_FILE}"
  DEPENDS
    "${SECS_RPC_PROTO_FILE}"
    ${SECS_RPC_PROTOC_DEP}
  COMMENT "Generating secs RPC protobuf sources"
  VERBATIM
)

add_library(secs_rpc
  "${SECS_RPC_GENERATED_SOURCE}"
  server.cpp
)
add_library(secs::rpc ALIAS secs_rpc)
set_target_properties(secs_rpc PROPERTIES EXPORT_NAME rpc)
target_compile_features(secs_rpc PUBLIC cxx_std_20)
target_compile_definitions(secs_rpc PRIVATE
  SECS_RPC_PROJECT_VERSION="${PROJECT_VERSION}"
)

target_include_directories(secs_rpc
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${SECS_RPC_GENERATED_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(secs_rpc PUBLIC secs_rpc_deps)

if(MSVC)
  target_compile_options(secs_rpc PRIVATE /W4)
  if(SECS_ENABLE_WERROR)
    target_compile_options(secs_rpc PRIVATE /WX)
  endif()
else()
  target_compile_options(secs_rpc PRIVATE -Wall -Wextra -Wpedantic)
  if(SECS_ENABLE_WERROR)
    target_compile_options(secs_rpc PRIVATE -Werror)
  endif()
endif()

if(SECS_ENABLE_INSTALL)
  install(FILES "${SECS_RPC_GENERATED_HEADER}"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/secs/rpc/v1
  )
endif()
