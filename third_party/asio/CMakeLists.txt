cmake_minimum_required(VERSION 3.20)

# 说明：此目录预期放置 standalone asio 源码（例如 submodule）。
# 典型结构：third_party/asio/asio/asio/include/asio.hpp
add_library(third_party_asio INTERFACE)
target_include_directories(third_party_asio INTERFACE "${CMAKE_CURRENT_LIST_DIR}/asio/asio/include")
target_compile_definitions(third_party_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
