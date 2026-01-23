function(secs_embed_text_as_c)
  set(options NULL_TERMINATE)
  set(oneValueArgs TARGET INPUT OUTPUT VAR)
  cmake_parse_arguments(SECS_EMBED "${options}" "${oneValueArgs}" "" ${ARGN})

  if(NOT SECS_EMBED_TARGET)
    message(FATAL_ERROR "secs_embed_text_as_c: TARGET is required")
  endif()
  if(NOT SECS_EMBED_INPUT)
    message(FATAL_ERROR "secs_embed_text_as_c: INPUT is required")
  endif()
  if(NOT SECS_EMBED_OUTPUT)
    message(FATAL_ERROR "secs_embed_text_as_c: OUTPUT is required")
  endif()
  if(NOT SECS_EMBED_VAR)
    message(FATAL_ERROR "secs_embed_text_as_c: VAR is required")
  endif()

  get_filename_component(_in_abs "${SECS_EMBED_INPUT}" ABSOLUTE)
  get_filename_component(_out_abs "${SECS_EMBED_OUTPUT}" ABSOLUTE)
  get_filename_component(_out_dir "${_out_abs}" DIRECTORY)

  set(_py "${PROJECT_SOURCE_DIR}/tools/embed_text_as_c.py")
  if(NOT EXISTS "${_py}")
    message(FATAL_ERROR "secs_embed_text_as_c: generator script not found: ${_py}")
  endif()

  set(_args "--input" "${_in_abs}" "--output" "${_out_abs}" "--var" "${SECS_EMBED_VAR}")
  if(SECS_EMBED_NULL_TERMINATE)
    list(APPEND _args "--null-terminate")
  endif()

  add_custom_command(
    OUTPUT "${_out_abs}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${_py}" ${_args}
    DEPENDS "${_in_abs}" "${_py}"
    VERBATIM
    COMMENT "Embedding ${SECS_EMBED_INPUT} -> ${SECS_EMBED_OUTPUT}"
  )

  target_sources(${SECS_EMBED_TARGET} PRIVATE "${_out_abs}")
  target_include_directories(${SECS_EMBED_TARGET} PRIVATE "${_out_dir}")
endfunction()
