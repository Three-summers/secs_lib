#include "bench_main.hpp"
#include "secs/core/buffer.hpp"

#include <cstring>
#include <vector>

using namespace secs;
using namespace secs::core;

static void bench_buffer_large_capacity() {
    // 验证 64MB 大容量扩展性能（测试 kDefaultFixedBufferMaxCapacity）
    constexpr std::size_t target_size = 64 * 1024 * 1024; // 64MB（兆字节）
    constexpr std::size_t chunk_size = 1024 * 1024;       // 1MB 块

    FixedBuffer buf;
    std::vector<byte> chunk(chunk_size, 0xAB);

    BENCH_RUN("Buffer: 64MB expansion (1MB chunks)", target_size, 3, {
        buf.clear();
        for (std::size_t written = 0; written < target_size;
             written += chunk_size) {
            auto ec = buf.append(bytes_view{chunk.data(), chunk.size()});
            if (ec) {
                std::cerr << "Append failed at " << written << " bytes\n";
                break;
            }
        }
    });
}

static void bench_buffer_small_append() {
    // 小块高频 append：1KB x 1000 次
    constexpr std::size_t chunk_size = 1024;
    constexpr std::size_t iterations = 1000;
    constexpr std::size_t total_size = chunk_size * iterations;

    FixedBuffer buf;
    std::vector<byte> chunk(chunk_size, 0xCD);

    BENCH_RUN("Buffer: Small append (1KB x 1000)", total_size, 3, {
        buf.clear();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto ec = buf.append(bytes_view{chunk.data(), chunk.size()});
            if (ec) {
                std::cerr << "Small append failed at iteration " << i << "\n";
                break;
            }
        }
    });
}

static void bench_buffer_large_append() {
    // 大块 append：1MB x 64 次
    constexpr std::size_t chunk_size = 1024 * 1024;
    constexpr std::size_t iterations = 64;
    constexpr std::size_t total_size = chunk_size * iterations;

    FixedBuffer buf;
    std::vector<byte> chunk(chunk_size, 0xEF);

    BENCH_RUN("Buffer: Large append (1MB x 64)", total_size, 3, {
        buf.clear();
        for (std::size_t i = 0; i < iterations; ++i) {
            auto ec = buf.append(bytes_view{chunk.data(), chunk.size()});
            if (ec) {
                std::cerr << "Large append failed at iteration " << i << "\n";
                break;
            }
        }
    });
}

static void bench_buffer_compact() {
    // Compact 操作性能：模拟流式读写场景
    constexpr std::size_t buffer_size = 8 * 1024 * 1024; // 8MB（兆字节）
    constexpr std::size_t chunk_size = 64 * 1024;        // 64KB（千字节）
    constexpr std::size_t consume_size = 32 * 1024;      // 32KB（千字节）

    FixedBuffer buf;
    std::vector<byte> chunk(chunk_size, 0x42);

    BENCH_RUN("Buffer: Compact operations", buffer_size, 3, {
        buf.clear();

        // 填充到接近容量
        for (std::size_t written = 0; written < buffer_size;
             written += chunk_size) {
            auto ec = buf.append(bytes_view{chunk.data(), chunk.size()});
            if (ec)
                break;
        }

        // 模拟流式处理：消费一半，compact，继续写入
        for (int i = 0; i < 10; ++i) {
            buf.consume(buf.size() / 2);
            buf.compact();
            buf.append(bytes_view{chunk.data(), chunk.size()});
        }
    });
}

static void bench_buffer_writable_commit() {
    // 直接写入 writable_bytes + commit 性能
    constexpr std::size_t total_size = 16 * 1024 * 1024; // 16MB（兆字节）
    constexpr std::size_t chunk_size = 4096;

    FixedBuffer buf;

    BENCH_RUN("Buffer: writable_bytes + commit", total_size, 3, {
        buf.clear();

        for (std::size_t written = 0; written < total_size;) {
            auto writable = buf.writable_bytes();
            if (writable.empty()) {
                buf.reserve(buf.capacity() * 2);
                continue;
            }

            std::size_t to_write = std::min(chunk_size, writable.size());
            std::memset(writable.data(), 0x55, to_write);
            buf.commit(to_write);
            written += to_write;
        }
    });
}

int main() {
    bench_buffer_large_capacity();
    bench_buffer_small_append();
    bench_buffer_large_append();
    bench_buffer_compact();
    bench_buffer_writable_commit();

    secs::benchmarks::print_results();
    return 0;
}
