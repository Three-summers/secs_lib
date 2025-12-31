#include "bench_main.hpp"
#include "secs/hsms/message.hpp"

#include <vector>

using namespace secs;
using namespace secs::core;
using namespace secs::hsms;

static void bench_hsms_max_payload() {
  // 16MB payload 编解码（验证 kMaxPayloadSize）
  constexpr std::size_t payload_size = 16 * 1024 * 1024;

  std::vector<byte> body(payload_size, 0xAA);
  Message msg = make_data_message(
    0x1234,        // session_id
    1,             // stream
    1,             // function
    false,         // w_bit
    0x87654321,    // system_bytes
    bytes_view{body.data(), body.size()});

  std::vector<byte> encoded;

  BENCH_RUN("HSMS: Encode 16MB payload", payload_size, 3, {
    encoded = encode_frame(msg);
  });

  // Decode benchmark
  std::size_t frame_size = encoded.size();
  BENCH_RUN("HSMS: Decode 16MB payload", frame_size, 3, {
    Message decoded;
    std::size_t consumed = 0;
    auto ec = decode_frame(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
      std::cerr << "Decode failed: " << ec.message() << "\n";
    }
  });
}

static void bench_hsms_small_messages() {
  // 小消息吞吐量测试：1000 条消息
  constexpr std::size_t message_count = 1000;
  constexpr std::size_t body_size = 128;

  std::vector<byte> body(body_size, 0xBB);
  std::vector<Message> messages;
  messages.reserve(message_count);

  for (std::size_t i = 0; i < message_count; ++i) {
    messages.push_back(make_data_message(
      static_cast<std::uint16_t>(i % 65536),
      static_cast<std::uint8_t>((i % 127) + 1),
      static_cast<std::uint8_t>(i % 256),
      (i % 2) == 0,
      static_cast<std::uint32_t>(i),
      bytes_view{body.data(), body.size()}));
  }

  std::vector<std::vector<byte>> encoded_frames;
  std::size_t total_size = 0;

  BENCH_RUN("HSMS: Encode 1000 small messages", message_count * body_size, 3, {
    encoded_frames.clear();
    encoded_frames.reserve(message_count);
    total_size = 0;
    for (const auto& msg : messages) {
      auto frame = encode_frame(msg);
      total_size += frame.size();
      encoded_frames.push_back(std::move(frame));
    }
  });

  // Decode benchmark
  BENCH_RUN("HSMS: Decode 1000 small messages", total_size, 3, {
    for (const auto& frame : encoded_frames) {
      Message decoded;
      std::size_t consumed = 0;
      auto ec = decode_frame(bytes_view{frame.data(), frame.size()}, decoded, consumed);
      if (ec) {
        std::cerr << "Decode failed: " << ec.message() << "\n";
        break;
      }
    }
  });
}

static void bench_hsms_control_messages() {
  // 控制消息性能测试
  constexpr std::size_t message_count = 10000;

  std::vector<Message> select_reqs;
  select_reqs.reserve(message_count);
  for (std::size_t i = 0; i < message_count; ++i) {
    select_reqs.push_back(make_select_req(
      static_cast<std::uint16_t>(i % 65536),
      static_cast<std::uint32_t>(i)));
  }

  std::vector<std::vector<byte>> encoded;

  BENCH_RUN("HSMS: Encode 10000 select_req", message_count * kHeaderSize, 3, {
    encoded.clear();
    encoded.reserve(message_count);
    for (const auto& msg : select_reqs) {
      encoded.push_back(encode_frame(msg));
    }
  });

  std::size_t total_size = 0;
  for (const auto& frame : encoded) {
    total_size += frame.size();
  }

  BENCH_RUN("HSMS: Decode 10000 select_req", total_size, 3, {
    for (const auto& frame : encoded) {
      Message decoded;
      std::size_t consumed = 0;
      auto ec = decode_frame(bytes_view{frame.data(), frame.size()}, decoded, consumed);
      if (ec) {
        std::cerr << "Decode failed: " << ec.message() << "\n";
        break;
      }
    }
  });
}

static void bench_hsms_various_sizes() {
  // 不同大小的消息性能对比
  std::vector<std::size_t> sizes = {
    64,              // 64B
    1024,            // 1KB
    64 * 1024,       // 64KB
    1024 * 1024,     // 1MB
    4 * 1024 * 1024  // 4MB
  };

  for (std::size_t size : sizes) {
    std::vector<byte> body(size, 0xCC);
    Message msg = make_data_message(
      0x1000,
      10,
      20,
      true,
      0x12345678,
      bytes_view{body.data(), body.size()});

    std::vector<byte> encoded;

    std::string bench_name = "HSMS: Encode message (" +
      std::to_string(size >= 1024 * 1024 ? size / (1024 * 1024) :
                     size >= 1024 ? size / 1024 : size) +
      (size >= 1024 * 1024 ? "MB)" : size >= 1024 ? "KB)" : "B)");

    BENCH_RUN(bench_name.c_str(), size, 3, {
      encoded = encode_frame(msg);
    });
  }
}

static void bench_hsms_decode_payload_only() {
  // decode_payload 性能测试（不含 4B length field）
  constexpr std::size_t payload_size = 8 * 1024 * 1024;  // 8MB

  std::vector<byte> body(payload_size, 0xDD);
  Message msg = make_data_message(
    0x2000,
    5,
    10,
    false,
    0xABCDEF00,
    bytes_view{body.data(), body.size()});

  auto full_frame = encode_frame(msg);
  // 跳过前 4 字节的 length field
  bytes_view payload{full_frame.data() + kLengthFieldSize, full_frame.size() - kLengthFieldSize};

  BENCH_RUN("HSMS: decode_payload (8MB)", payload.size(), 3, {
    Message decoded;
    auto ec = decode_payload(payload, decoded);
    if (ec) {
      std::cerr << "decode_payload failed: " << ec.message() << "\n";
    }
  });
}

int main() {
  bench_hsms_max_payload();
  bench_hsms_small_messages();
  bench_hsms_control_messages();
  bench_hsms_various_sizes();
  bench_hsms_decode_payload_only();

  secs::benchmarks::print_results();
  return 0;
}
