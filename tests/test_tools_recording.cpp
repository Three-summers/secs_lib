/**
 * @file test_tools_recording.cpp
 * @brief tools::MessageRecorder / MessagePlayer JSONL 编解码单元测试
 */

#include "secs/tools/recording.hpp"
#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using secs::core::byte;
using secs::core::make_error_code;
using secs::tools::MessagePlayer;
using secs::tools::MessageRecorder;
using secs::tools::RecordedMessage;

static std::string make_temp_path(std::string_view prefix) {
    const auto base = std::filesystem::temp_directory_path();
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = base / (std::string(prefix) + "_" + std::to_string(static_cast<long long>(n)) +
                     ".jsonl");
    return p.string();
}

void test_recorded_message_jsonl_roundtrip_basic() {
    RecordedMessage m{};
    m.timestamp_us = 42;
    m.is_tx = true;
    m.stream = 1;
    m.function = 1;
    m.w_bit = true;
    m.system_bytes = 0x01020304u;
    m.body = std::vector<byte>{0x00, 0xFF, 0x1A};
    m.decoded_sml = std::string(R"(S1F1 W. "quote" \\ slash)");

    const auto line = MessagePlayer::to_jsonl_line(m);
    RecordedMessage parsed{};
    TEST_EXPECT_OK(MessagePlayer::parse_jsonl_line(line, parsed));

    TEST_EXPECT_EQ(parsed.timestamp_us, m.timestamp_us);
    TEST_EXPECT_EQ(parsed.is_tx, m.is_tx);
    TEST_EXPECT_EQ(parsed.stream, m.stream);
    TEST_EXPECT_EQ(parsed.function, m.function);
    TEST_EXPECT_EQ(parsed.w_bit, m.w_bit);
    TEST_EXPECT_EQ(parsed.system_bytes, m.system_bytes);
    TEST_EXPECT_EQ(parsed.body, m.body);
    TEST_EXPECT(parsed.decoded_sml.has_value());
    TEST_EXPECT_EQ(*parsed.decoded_sml, *m.decoded_sml);
}

void test_recorded_message_jsonl_alias_keys() {
    const std::string line =
        R"({"ts":1,"direction":"TX","stream":1,"function":2,"w_bit":false,"system_bytes":123,"body":"00ff"})";
    RecordedMessage parsed{};
    TEST_EXPECT_OK(MessagePlayer::parse_jsonl_line(line, parsed));
    TEST_EXPECT_EQ(parsed.timestamp_us, 1u);
    TEST_EXPECT(parsed.is_tx);
    TEST_EXPECT_EQ(parsed.stream, 1u);
    TEST_EXPECT_EQ(parsed.function, 2u);
    TEST_EXPECT(!parsed.w_bit);
    TEST_EXPECT_EQ(parsed.system_bytes, 123u);
    TEST_EXPECT_EQ(parsed.body.size(), 2u);
    TEST_EXPECT_EQ(parsed.body[0], 0x00u);
    TEST_EXPECT_EQ(parsed.body[1], 0xFFu);
}

void test_recorded_message_jsonl_rejects_invalid_direction() {
    const std::string line =
        R"({"ts_us":1,"dir":"BAD","s":1,"f":1,"w":false,"sb":1,"body_hex":""})";
    RecordedMessage parsed{};
    const auto ec = MessagePlayer::parse_jsonl_line(line, parsed);
    TEST_EXPECT(static_cast<bool>(ec));
}

void test_message_recorder_and_player_end_to_end_file() {
    const auto path = make_temp_path("secs_tools_recording");

    {
        MessageRecorder rec(path);
        TEST_EXPECT(rec.is_open());

        rec.set_include_sml_header(true);

        secs::protocol::DataMessage tx{};
        tx.stream = 1;
        tx.function = 1;
        tx.w_bit = true;
        tx.system_bytes = 123;
        tx.body = std::vector<byte>{0x01, 0x02, 0x03};

        secs::protocol::DataMessage rx{};
        rx.stream = 1;
        rx.function = 2;
        rx.w_bit = false;
        rx.system_bytes = 123;
        rx.body = std::vector<byte>{0xAA, 0xBB};

        rec.record_tx(tx);
        rec.record_rx(rx);
        rec.flush();
        TEST_EXPECT_OK(rec.last_error());
    }

    {
        MessagePlayer player(path);
        TEST_EXPECT(player.is_open());

        auto a = player.next_message();
        TEST_EXPECT(a.has_value());
        TEST_EXPECT(a->is_tx);
        TEST_EXPECT_EQ(a->stream, 1u);
        TEST_EXPECT_EQ(a->function, 1u);
        TEST_EXPECT(a->w_bit);
        TEST_EXPECT_EQ(a->system_bytes, 123u);
        TEST_EXPECT_EQ(a->body.size(), 3u);
        TEST_EXPECT(a->decoded_sml.has_value());
        TEST_EXPECT_EQ(*a->decoded_sml, std::string("S1F1 W."));

        auto b = player.next_message();
        TEST_EXPECT(b.has_value());
        TEST_EXPECT(!b->is_tx);
        TEST_EXPECT_EQ(b->stream, 1u);
        TEST_EXPECT_EQ(b->function, 2u);
        TEST_EXPECT(!b->w_bit);
        TEST_EXPECT_EQ(b->system_bytes, 123u);
        TEST_EXPECT_EQ(b->body.size(), 2u);
        TEST_EXPECT(b->decoded_sml.has_value());
        TEST_EXPECT_EQ(*b->decoded_sml, std::string("S1F2."));

        auto c = player.next_message();
        TEST_EXPECT(!c.has_value());
        TEST_EXPECT(!player.last_error());
    }

    std::error_code rm_ec{};
    std::filesystem::remove(path, rm_ec);
}

void test_message_recorder_filter_exception_sets_error() {
    const auto path = make_temp_path("secs_tools_recording_filter");

    MessageRecorder rec(path);
    TEST_EXPECT(rec.is_open());

    rec.set_filter([](const secs::protocol::DataMessage &) -> bool {
        throw std::runtime_error("boom");
    });

    secs::protocol::DataMessage msg{};
    msg.stream = 1;
    msg.function = 1;
    msg.w_bit = false;
    msg.system_bytes = 1;
    msg.body = std::vector<byte>{0x01};

    rec.record_tx(msg);

    const auto ec = rec.last_error();
    TEST_EXPECT(static_cast<bool>(ec));
    TEST_EXPECT_EQ(ec, make_error_code(secs::core::errc::invalid_argument));

    std::error_code rm_ec{};
    std::filesystem::remove(path, rm_ec);
}

void test_recorded_message_jsonl_rejects_out_of_range_fields() {
    const std::string line =
        R"({"ts_us":1,"dir":"TX","s":256,"f":1,"w":false,"sb":1,"body_hex":""})";
    RecordedMessage parsed{};
    const auto ec = MessagePlayer::parse_jsonl_line(line, parsed);
    TEST_EXPECT(static_cast<bool>(ec));
}

void test_recorded_message_jsonl_allows_unknown_null_field() {
    const std::string line =
        R"({"ts_us":1,"dir":"RX","s":1,"f":2,"w":false,"sb":3,"body_hex":"00ff","extra":null})";
    RecordedMessage parsed{};
    TEST_EXPECT_OK(MessagePlayer::parse_jsonl_line(line, parsed));
    TEST_EXPECT(!parsed.is_tx);
    TEST_EXPECT_EQ(parsed.body.size(), 2u);
}

void test_recorded_message_jsonl_rejects_invalid_unicode_escape() {
    const std::string line =
        R"({"ts_us":1,"dir":"TX","s":1,"f":1,"w":false,"sb":1,"body_hex":"","sml":"\u1234"})";
    RecordedMessage parsed{};
    const auto ec = MessagePlayer::parse_jsonl_line(line, parsed);
    TEST_EXPECT(static_cast<bool>(ec));
}

} // namespace

int main() {
    test_recorded_message_jsonl_roundtrip_basic();
    test_recorded_message_jsonl_alias_keys();
    test_recorded_message_jsonl_rejects_invalid_direction();
    test_message_recorder_and_player_end_to_end_file();
    test_message_recorder_filter_exception_sets_error();
    test_recorded_message_jsonl_rejects_out_of_range_fields();
    test_recorded_message_jsonl_allows_unknown_null_field();
    test_recorded_message_jsonl_rejects_invalid_unicode_escape();
    return secs::tests::run_and_report();
}
