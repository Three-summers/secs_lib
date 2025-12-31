#pragma once

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace secs::benchmarks {

struct BenchmarkResult {
    std::string_view name;
    std::size_t data_size;
    double elapsed_ms;
    double throughput_mbps;
};

class BenchmarkTimer {
public:
    void start() { start_ = std::chrono::high_resolution_clock::now(); }

    void stop() { end_ = std::chrono::high_resolution_clock::now(); }

    [[nodiscard]] double elapsed_ms() const {
        auto duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_);
        return static_cast<double>(duration.count()) / 1'000'000.0;
    }

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
    std::chrono::time_point<std::chrono::high_resolution_clock> end_;
};

inline std::vector<BenchmarkResult> &results() {
    static std::vector<BenchmarkResult> results_;
    return results_;
}

template <typename Func>
inline void run_benchmark(std::string_view name,
                          std::size_t data_size,
                          int iterations,
                          Func &&func) {

    std::vector<double> timings;
    timings.reserve(iterations);

    // 重复执行多次，降低单次抖动影响（取平均值作为结果）
    for (int i = 0; i < iterations; ++i) {
        BenchmarkTimer timer;
        timer.start();
        func();
        timer.stop();
        timings.push_back(timer.elapsed_ms());
    }

    // 计算平均耗时
    double total_ms = 0.0;
    for (double t : timings) {
        total_ms += t;
    }
    double avg_ms = total_ms / iterations;

    // 计算吞吐（MB/s）
    double throughput_mbps = 0.0;
    if (avg_ms > 0.0) {
        double seconds = avg_ms / 1000.0;
        double mb = static_cast<double>(data_size) / (1024.0 * 1024.0);
        throughput_mbps = mb / seconds;
    }

    results().push_back({name, data_size, avg_ms, throughput_mbps});
}

inline void print_results() {
    std::cout << "\n";
    std::cout << std::string(100, '=') << "\n";
    std::cout << "BENCHMARK RESULTS\n";
    std::cout << std::string(100, '=') << "\n";
    std::cout << std::left << std::setw(50) << "Benchmark" << std::setw(15)
              << "Size" << std::setw(15) << "Time (ms)" << std::setw(20)
              << "Throughput (MB/s)"
              << "\n";
    std::cout << std::string(100, '-') << "\n";

    for (const auto &result : results()) {
        std::cout << std::left << std::setw(50) << result.name;

        // 格式化大小显示
        if (result.data_size >= 1024 * 1024) {
            std::cout << std::setw(15)
                      << (std::to_string(result.data_size / (1024 * 1024)) +
                          " MB");
        } else if (result.data_size >= 1024) {
            std::cout << std::setw(15)
                      << (std::to_string(result.data_size / 1024) + " KB");
        } else {
            std::cout << std::setw(15)
                      << (std::to_string(result.data_size) + " B");
        }

        std::cout << std::fixed << std::setprecision(3) << std::setw(15)
                  << result.elapsed_ms;

        if (result.throughput_mbps > 0.0) {
            std::cout << std::setw(20) << result.throughput_mbps;
        } else {
            std::cout << std::setw(20) << "N/A";
        }

        std::cout << "\n";
    }

    std::cout << std::string(100, '=') << "\n\n";
}

} // namespace secs::benchmarks

#define BENCH_RUN(name, size, iterations, code)                                \
    ::secs::benchmarks::run_benchmark(name, size, iterations, [&]() { code; })
