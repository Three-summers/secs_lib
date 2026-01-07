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
      --merge-lines
      --exclude-noncode-lines
      --html --html-details
      -o ${PROJECT_BINARY_DIR}/coverage.html
      # 兼容两种路径形态：
      # - 绝对路径：/home/.../tests/...
      # - 相对路径：tests/...
      #
      # 注意：不要在这里使用带括号/管道符的正则（例如 "(^|.*/)"），因为
      # Makefile 生成器会把它们以未加引号的形式交给 /bin/sh，导致语法错误。
      --exclude '^tests/.*'
      --exclude '.*/tests/.*'
      --exclude '^c_dump/.*'
      --exclude '.*/c_dump/.*'
      --exclude '^third_party/.*'
      --exclude '.*/third_party/.*'
      # FetchContent/_deps（例如 spdlog）：不计入库覆盖率口径
      --exclude '^_deps/.*'
      --exclude '.*/_deps/.*'
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    USES_TERMINAL
  )
endfunction()
