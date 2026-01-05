#include "bench_main.hpp"

#include "secs/ii/item.hpp"
#include "secs/sml/runtime.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace secs;
using namespace secs::ii;
using namespace secs::sml;

static std::string make_large_sml(std::size_t message_count) {
    std::string out;
    out.reserve(message_count * 64);

    for (std::size_t i = 0; i < message_count; ++i) {
        const std::uint32_t idx = static_cast<std::uint32_t>(i);
        const std::uint8_t stream = static_cast<std::uint8_t>((idx % 127u) + 1u);
        const std::uint8_t function = static_cast<std::uint8_t>((idx % 255u) + 1u);

        out += "m";
        out += std::to_string(i);
        out += ": S";
        out += std::to_string(stream);
        out += "F";
        out += std::to_string(function);
        out += " <L <A \"HELLO\">>.\n";
    }

    out += "req: S1F1 W <L <A \"PING\">>.\n";
    out += "rsp: S1F2 <A \"PONG\">.\n";
    out += "if (req) rsp.\n";
    return out;
}

static void bench_sml_load(std::size_t message_count) {
    const auto source = make_large_sml(message_count);

    BENCH_RUN("SML: load", source.size(), 5, {
        Runtime rt;
        auto ec = rt.load(source);
        if (ec) {
            std::cerr << "SML load failed: " << ec.message() << "\n";
        }
    });
}

static void bench_sml_match(std::size_t message_count) {
    const auto source = make_large_sml(message_count);

    Runtime rt;
    auto ec = rt.load(source);
    if (ec) {
        std::cerr << "SML load failed: " << ec.message() << "\n";
        return;
    }

    const Item req_body = Item::list({Item::ascii("PING")});

    // 先做一次探测，避免在 benchmark 循环里持续输出错误干扰测量。
    {
        auto matched = rt.match_response(1, 1, req_body);
        if (!matched || *matched != "rsp") {
            std::cerr << "SML match_response mismatch\n";
            return;
        }
    }

    constexpr int inner_loops = 10000;
    constexpr std::size_t approx_body_bytes = 4; // "PING"
    BENCH_RUN("SML: match_response (S1F1)",
              approx_body_bytes * static_cast<std::size_t>(inner_loops),
              5,
              {
                  std::size_t hits = 0;
                  for (int i = 0; i < inner_loops; ++i) {
                      if (rt.match_response(1, 1, req_body)) {
                          ++hits;
                      }
                  }
                  if (hits == 0) {
                      std::cerr << "SML match_response unexpected miss\n";
                  }
    });
}

int main() {
    constexpr std::size_t message_count = 1000;

    bench_sml_load(message_count);
    bench_sml_match(message_count);

    secs::benchmarks::print_results();
    return 0;
}
