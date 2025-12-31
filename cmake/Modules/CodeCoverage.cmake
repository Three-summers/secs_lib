include_guard(GLOBAL)

# 只对库添加覆盖率编译标志（不影响链接）
function(secs_enable_coverage_compile target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
  endif()
endfunction()

# 对测试可执行文件添加完整覆盖率支持（编译+链接）
function(secs_enable_coverage target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
      target_link_options(${target} PRIVATE --coverage)
    else()
      target_link_libraries(${target} PRIVATE --coverage)
    endif()
  endif()
endfunction()

function(secs_add_coverage_target target_name)
  find_program(GCOVR_EXECUTABLE gcovr)
  if(NOT GCOVR_EXECUTABLE)
    message(FATAL_ERROR "gcovr not found. Install via: pip install gcovr")
  endif()

  add_custom_target(${target_name}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND ${GCOVR_EXECUTABLE}
      -r ${PROJECT_SOURCE_DIR}
      --merge-mode-functions=merge-use-line-min
      --html --html-details
      -o ${PROJECT_BINARY_DIR}/coverage.html
      --exclude '.*/tests/.*'
      --exclude '.*/third_party/.*'
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    USES_TERMINAL
  )
endfunction()
