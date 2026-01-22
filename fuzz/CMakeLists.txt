if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "SECS_ENABLE_FUZZING requires Clang (libFuzzer)")
endif()

function(secs_add_fuzzer target)
  add_executable(${target} ${ARGN})
  target_compile_features(${target} PRIVATE cxx_std_20)

  # libFuzzer + ASan/UBSan：面向本地开发/OSS-Fuzz 风格使用。
  target_compile_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
  )
  target_link_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
  )
endfunction()

secs_add_fuzzer(fuzz_ii_decode_one fuzz_ii_decode_one.cpp)
target_link_libraries(fuzz_ii_decode_one PRIVATE secs_ii)

secs_add_fuzzer(fuzz_hsms_decode_payload fuzz_hsms_decode_payload.cpp)
target_link_libraries(fuzz_hsms_decode_payload PRIVATE secs_hsms)

secs_add_fuzzer(fuzz_sml_parse fuzz_sml_parse.cpp)
target_link_libraries(fuzz_sml_parse PRIVATE secs_sml)

