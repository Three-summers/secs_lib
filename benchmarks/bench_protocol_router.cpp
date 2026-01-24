#include "bench_main.hpp"

#include "secs/protocol/router.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using secs::protocol::Router;

static volatile std::uint64_t g_sink = 0;

static secs::protocol::Handler make_noop_handler() {
    return [](const DataMessage &) -> asio::awaitable<HandlerResult> {
        co_return HandlerResult{std::error_code{}, {}};
    };
}

static std::vector<std::pair<std::uint8_t, std::uint8_t>>
make_queries(std::size_t streams, std::size_t functions, std::size_t n) {
    std::vector<std::pair<std::uint8_t, std::uint8_t>> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = static_cast<std::uint8_t>((i % streams) + 1u);
        const auto f = static_cast<std::uint8_t>((i % functions) + 1u);
        out.emplace_back(s, f);
    }
    return out;
}

static void bench_router_exact(std::size_t streams, std::size_t functions) {
    Router router;
    const auto handler = make_noop_handler();
    for (std::size_t s = 1; s <= streams; ++s) {
        for (std::size_t f = 1; f <= functions; ++f) {
            router.set(static_cast<std::uint8_t>(s),
                       static_cast<std::uint8_t>(f),
                       handler);
        }
    }

    const auto queries = make_queries(streams, functions, 1024);
    constexpr int rounds = 2000;
    const auto lookups =
        static_cast<std::size_t>(rounds) * static_cast<std::size_t>(queries.size());

    /* 预热 */
    {
        const auto h = router.find(queries[0].first, queries[0].second);
        if (!h.has_value()) {
            std::cerr << "Router exact preflight miss\n";
        }
    }

    BENCH_RUN(std::string("Protocol: Router find exact (") +
                  std::to_string(streams * functions) + " handlers)",
              lookups * sizeof(std::uint16_t),
              5,
              {
                  std::size_t hits = 0;
                  for (int r = 0; r < rounds; ++r) {
                      for (const auto &q : queries) {
                          if (router.find(q.first, q.second).has_value()) {
                              ++hits;
                          }
                      }
                  }
                  g_sink ^= hits;
              });
}

static void bench_router_stream_default(std::size_t streams, std::size_t functions) {
    Router router;
    const auto handler = make_noop_handler();
    for (std::size_t s = 1; s <= streams; ++s) {
        router.set_stream_default(static_cast<std::uint8_t>(s), handler);
    }

    const auto queries = make_queries(streams, functions, 1024);
    constexpr int rounds = 4000;
    const auto lookups =
        static_cast<std::size_t>(rounds) * static_cast<std::size_t>(queries.size());

    /* 预热 */
    {
        const auto h = router.find(queries[0].first, queries[0].second);
        if (!h.has_value()) {
            std::cerr << "Router stream-default preflight miss\n";
        }
    }

    BENCH_RUN(std::string("Protocol: Router find stream-default (") +
                  std::to_string(streams) + " streams)",
              lookups * sizeof(std::uint16_t),
              5,
              {
                  std::size_t hits = 0;
                  for (int r = 0; r < rounds; ++r) {
                      for (const auto &q : queries) {
                          if (router.find(q.first, q.second).has_value()) {
                              ++hits;
                          }
                      }
                  }
                  g_sink ^= hits;
              });
}

static void bench_router_default(std::size_t streams, std::size_t functions) {
    Router router;
    router.set_default(make_noop_handler());

    const auto queries = make_queries(streams, functions, 1024);
    constexpr int rounds = 4000;
    const auto lookups =
        static_cast<std::size_t>(rounds) * static_cast<std::size_t>(queries.size());

    BENCH_RUN("Protocol: Router find default",
              lookups * sizeof(std::uint16_t),
              5,
              {
                  std::size_t hits = 0;
                  for (int r = 0; r < rounds; ++r) {
                      for (const auto &q : queries) {
                          if (router.find(q.first, q.second).has_value()) {
                              ++hits;
                          }
                      }
                  }
                  g_sink ^= hits;
              });
}

static void bench_router_miss(std::size_t streams, std::size_t functions) {
    Router router;

    const auto queries = make_queries(streams, functions, 1024);
    constexpr int rounds = 4000;
    const auto lookups =
        static_cast<std::size_t>(rounds) * static_cast<std::size_t>(queries.size());

    BENCH_RUN("Protocol: Router find miss",
              lookups * sizeof(std::uint16_t),
              5,
              {
                  std::size_t misses = 0;
                  for (int r = 0; r < rounds; ++r) {
                      for (const auto &q : queries) {
                          if (!router.find(q.first, q.second).has_value()) {
                              ++misses;
                          }
                      }
                  }
                  g_sink ^= misses;
              });
}

} // namespace

int main() {
    bench_router_exact(1, 255);  // 255 handlers
    bench_router_exact(16, 255); // 4080 handlers
    bench_router_stream_default(16, 255);
    bench_router_default(16, 255);
    bench_router_miss(16, 255);

    secs::benchmarks::print_results();
    return g_sink == 0 ? 0 : 0;
}

