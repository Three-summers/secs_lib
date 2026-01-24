#include "bench_main.hpp"

#include "secs/protocol/system_bytes.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

static volatile std::uint64_t g_sink = 0;

static void bench_system_bytes_allocate_release_single() {
    constexpr int loops = 500000;

    BENCH_RUN("Protocol: SystemBytes allocate+release (single)",
              static_cast<std::size_t>(loops) * sizeof(std::uint32_t),
              5,
              {
                  secs::protocol::SystemBytes sb;
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < loops; ++i) {
                      std::uint32_t out = 0;
                      const auto ec = sb.allocate(out);
                      if (ec) {
                          std::cerr << "SystemBytes allocate failed: "
                                    << ec.message() << "\n";
                          break;
                      }
                      sb.release(out);
                      checksum += out;
                  }
                  g_sink ^= checksum;
              });
}

static void bench_system_bytes_allocate_release_batch(std::size_t batch,
                                                     int rounds) {
    const auto total = batch * static_cast<std::size_t>(rounds);
    std::vector<std::uint32_t> ids(batch);

    BENCH_RUN(std::string("Protocol: SystemBytes allocate+release (batch=") +
                  std::to_string(batch) + ")",
              total * sizeof(std::uint32_t),
              5,
              {
                  secs::protocol::SystemBytes sb;
                  std::uint64_t checksum = 0;

                  for (int r = 0; r < rounds; ++r) {
                      for (std::size_t i = 0; i < batch; ++i) {
                          std::uint32_t out = 0;
                          const auto ec = sb.allocate(out);
                          if (ec) {
                              std::cerr << "SystemBytes allocate failed: "
                                        << ec.message() << "\n";
                              return;
                          }
                          ids[i] = out;
                          checksum += out;
                      }
                      for (std::size_t i = 0; i < batch; ++i) {
                          sb.release(ids[i]);
                      }
                  }

                  g_sink ^= checksum;
              });
}

} // namespace

int main() {
    bench_system_bytes_allocate_release_single();
    bench_system_bytes_allocate_release_batch(64, 5000);
    bench_system_bytes_allocate_release_batch(1024, 500);

    secs::benchmarks::print_results();
    return g_sink == 0 ? 0 : 0;
}

