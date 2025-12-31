# Benchmarks for SECS library

add_executable(bench_core_buffer bench_core_buffer.cpp)
target_link_libraries(bench_core_buffer PRIVATE secs::core)
target_include_directories(bench_core_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_secs2_codec bench_secs2_codec.cpp)
target_link_libraries(bench_secs2_codec PRIVATE secs::core secs::ii)
target_include_directories(bench_secs2_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bench_hsms_message bench_hsms_message.cpp)
target_link_libraries(bench_hsms_message PRIVATE secs::core secs::hsms)
target_include_directories(bench_hsms_message PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Compiler warnings for benchmarks
if(MSVC)
  target_compile_options(bench_core_buffer PRIVATE /W4)
  target_compile_options(bench_secs2_codec PRIVATE /W4)
  target_compile_options(bench_hsms_message PRIVATE /W4)
else()
  target_compile_options(bench_core_buffer PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(bench_secs2_codec PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(bench_hsms_message PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional: Add optimization flags for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(MSVC)
    target_compile_options(bench_core_buffer PRIVATE /O2)
    target_compile_options(bench_secs2_codec PRIVATE /O2)
    target_compile_options(bench_hsms_message PRIVATE /O2)
  else()
    target_compile_options(bench_core_buffer PRIVATE -O3)
    target_compile_options(bench_secs2_codec PRIVATE -O3)
    target_compile_options(bench_hsms_message PRIVATE -O3)
  endif()
endif()
