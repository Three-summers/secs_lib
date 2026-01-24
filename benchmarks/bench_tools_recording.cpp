#include "bench_main.hpp"

#include "secs/tools/recording.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using secs::core::byte;
using secs::tools::MessagePlayer;
using secs::tools::RecordedMessage;

static volatile std::uint64_t g_sink = 0;

static RecordedMessage make_msg(std::size_t body_n, bool with_sml) {
    RecordedMessage m{};
    m.timestamp_us = 123;
    m.is_tx = true;
    m.stream = 1;
    m.function = 2;
    m.w_bit = false;
    m.system_bytes = 0x01020304u;
    m.body.resize(body_n);
    for (std::size_t i = 0; i < body_n; ++i) {
        m.body[i] = static_cast<byte>(i & 0xFFu);
    }
    if (with_sml) {
        m.decoded_sml = std::string("S1F2.");
    }
    return m;
}

static std::string inject_peer_local_fields(std::string line) {
    const auto rb = line.rfind('}');
    if (rb == std::string::npos) {
        return line;
    }
    line.insert(rb,
                ",\"peer\":\"127.0.0.1:5000\",\"local\":\"0.0.0.0:1234\"");
    return line;
}

static void bench_parse(std::string_view name,
                        const std::string &line,
                        int inner_loops) {
    RecordedMessage out{};
    const auto pre_ec = MessagePlayer::parse_jsonl_line(line, out);
    if (pre_ec) {
        std::cerr << "preflight parse failed: " << pre_ec.message() << "\n";
        return;
    }

    const auto total_bytes =
        static_cast<std::size_t>(line.size()) * static_cast<std::size_t>(inner_loops);
    BENCH_RUN(std::string(name), total_bytes, 5, {
        std::uint64_t checksum = 0;
        RecordedMessage tmp{};
        for (int i = 0; i < inner_loops; ++i) {
            const auto ec = MessagePlayer::parse_jsonl_line(line, tmp);
            if (ec) {
                std::cerr << "parse failed: " << ec.message() << "\n";
                break;
            }
            checksum += tmp.body.size();
            checksum += tmp.stream;
            checksum += tmp.function;
            checksum += tmp.w_bit ? 1u : 0u;
            checksum += tmp.system_bytes;
        }
        g_sink ^= checksum;
    });
}

static void bench_serialize(std::string_view name,
                            const RecordedMessage &msg,
                            int inner_loops) {
    const auto sample = MessagePlayer::to_jsonl_line(msg);
    const auto total_bytes =
        static_cast<std::size_t>(sample.size()) * static_cast<std::size_t>(inner_loops);

    BENCH_RUN(std::string(name), total_bytes, 5, {
        std::uint64_t checksum = 0;
        for (int i = 0; i < inner_loops; ++i) {
            const auto line = MessagePlayer::to_jsonl_line(msg);
            checksum += line.size();
            if (!line.empty()) {
                checksum += static_cast<unsigned char>(line[0]);
                checksum += static_cast<unsigned char>(line.back());
            }
        }
        g_sink ^= checksum;
    });
}

static void bench_tools_recording_jsonl() {
    struct Case {
        std::size_t body_n;
        int parse_loops;
        int ser_loops;
    };
    const std::vector<Case> cases = {
        {0, 200000, 200000},
        {16, 100000, 100000},
        {256, 5000, 5000},
        {4096, 200, 200},
    };

    for (const auto &c : cases) {
        const auto msg = make_msg(c.body_n, false);
        const auto line = MessagePlayer::to_jsonl_line(msg);
        bench_parse(
            std::string("Tools: JSONL parse (body=") + std::to_string(c.body_n) +
                ")",
            line,
            c.parse_loops);
        bench_serialize(
            std::string("Tools: JSONL serialize (body=") +
                std::to_string(c.body_n) + ")",
            msg,
            c.ser_loops);
    }

    {
        const auto msg = make_msg(256, true);
        const auto line = MessagePlayer::to_jsonl_line(msg);
        bench_parse("Tools: JSONL parse (with sml)", line, 5000);
        bench_serialize("Tools: JSONL serialize (with sml)", msg, 5000);
    }
    {
        const auto msg = make_msg(256, false);
        const auto line = inject_peer_local_fields(MessagePlayer::to_jsonl_line(msg));
        bench_parse("Tools: JSONL parse (with peer/local)", line, 5000);
    }
    {
        const auto msg = make_msg(256, true);
        const auto line = inject_peer_local_fields(MessagePlayer::to_jsonl_line(msg));
        bench_parse("Tools: JSONL parse (sml + peer/local)", line, 5000);
    }
}

} // namespace

int main() {
    bench_tools_recording_jsonl();
    secs::benchmarks::print_results();
    return g_sink == 0 ? 0 : 0;
}
