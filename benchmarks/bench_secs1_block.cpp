#include "bench_main.hpp"

#include "secs/secs1/block.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace secs;
using namespace secs::core;
using namespace secs::secs1;

static std::vector<byte> make_payload(std::size_t n) {
    std::vector<byte> out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<byte>(i & 0xFFu);
    }
    return out;
}

static void bench_secs1_single_block() {
    // 单块：最大 244B payload（不触发分包）
    constexpr std::size_t payload_n = kMaxBlockDataSize;
    constexpr int inner_loops = 5000;

    Header header{};
    header.reverse_bit = false;
    header.device_id = 1;
    header.wait_bit = true;
    header.stream = 1;
    header.function = 1;
    header.end_bit = true;
    header.block_number = 1;
    header.system_bytes = 0x12345678u;

    const auto payload = make_payload(payload_n);

    std::vector<byte> frame;
    BENCH_RUN("SECS-I: Encode single block (244B)",
              payload_n * static_cast<std::size_t>(inner_loops),
              5,
              {
                  for (int i = 0; i < inner_loops; ++i) {
                      frame.clear();
                      auto ec = encode_block(
                          header,
                          bytes_view{payload.data(), payload.size()},
                          frame);
                      if (ec) {
                          std::cerr << "encode_block failed: " << ec.message()
                                    << "\n";
                          break;
                      }
                  }
    });

    BENCH_RUN("SECS-I: Decode single block (244B)",
              frame.size() * static_cast<std::size_t>(inner_loops),
              5,
              {
                  for (int i = 0; i < inner_loops; ++i) {
                      DecodedBlock decoded{};
                      auto ec = decode_block(
                          bytes_view{frame.data(), frame.size()}, decoded);
                      if (ec) {
                          std::cerr << "decode_block failed: " << ec.message()
                                    << "\n";
                          break;
                      }
                  }
    });
}

static void bench_secs1_fragment_and_reassemble() {
    // 多块：700B payload（触发 244B 分包）
    constexpr std::size_t payload_n = 700;
    constexpr int inner_loops = 500;

    Header base{};
    base.reverse_bit = false;
    base.device_id = 1;
    base.wait_bit = true;
    base.stream = 1;
    base.function = 13;
    base.system_bytes = 0xCAFEBABEu;

    const auto payload = make_payload(payload_n);

    std::vector<std::vector<byte>> frames;
    BENCH_RUN("SECS-I: Fragment message (700B)",
              payload.size() * static_cast<std::size_t>(inner_loops),
              5,
              {
                  for (int i = 0; i < inner_loops; ++i) {
                      frames = fragment_message(
                          base,
                          bytes_view{payload.data(), payload.size()});
                  }
    });

    std::size_t total_frame_bytes = 0;
    for (const auto &f : frames) {
        total_frame_bytes += f.size();
    }

    BENCH_RUN("SECS-I: Decode+reassemble (700B)",
              total_frame_bytes * static_cast<std::size_t>(inner_loops),
              5,
              {
                  for (int i = 0; i < inner_loops; ++i) {
                      Reassembler reasm{base.device_id};
                      for (const auto &f : frames) {
                          DecodedBlock decoded{};
                          auto ec = decode_block(
                              bytes_view{f.data(), f.size()}, decoded);
                          if (ec) {
                              std::cerr << "decode_block failed: "
                                        << ec.message() << "\n";
                              break;
                          }
                          ec = reasm.accept(decoded);
                          if (ec) {
                              std::cerr << "reassemble accept failed: "
                                        << ec.message() << "\n";
                              break;
                          }
                      }
                      if (!reasm.has_message() ||
                          reasm.message_body().size() != payload.size()) {
                          std::cerr << "reassembler mismatch\n";
                          break;
                      }
                  }
    });
}

int main() {
    bench_secs1_single_block();
    bench_secs1_fragment_and_reassemble();

    secs::benchmarks::print_results();
    return 0;
}
