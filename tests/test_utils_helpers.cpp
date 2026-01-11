#include "test_main.hpp"

#include <secs/core/common.hpp>
#include <secs/core/error.hpp>
#include <secs/ii/item.hpp>
#include <secs/protocol/session.hpp>
#include <secs/secs1/link.hpp>
#include <secs/secs1/state_machine.hpp>
#include <secs/utils/ii_helpers.hpp>
#include <secs/utils/protocol_helpers.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using secs::core::byte;
using secs::core::bytes_view;

using secs::ii::Item;
using secs::ii::List;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using secs::protocol::Session;
using secs::protocol::SessionOptions;

using secs::secs1::MemoryLink;
using secs::secs1::StateMachine;

void test_ii_helpers_roundtrip() {
    const Item input = Item::list({Item::ascii("OK"), Item::u4({100})});

    auto [enc_ec, bytes] = secs::utils::encode_item(input);
    TEST_EXPECT_OK(enc_ec);
    TEST_EXPECT(!bytes.empty());

    auto [dec_ec, decoded] = secs::utils::decode_one_item(
        bytes_view{bytes.data(), bytes.size()});
    TEST_EXPECT_OK(dec_ec);
    TEST_EXPECT(decoded.fully_consumed);
    TEST_EXPECT(decoded.item == input);
    TEST_EXPECT_EQ(decoded.consumed, bytes.size());

    // 空输入：返回 ok + nullopt
    auto [any_ec, any] = secs::utils::decode_one_item_if_any(bytes_view{});
    TEST_EXPECT_OK(any_ec);
    TEST_EXPECT(!any.has_value());
}

void test_protocol_helpers_request_decoded() {
    asio::io_context ioc;
    const auto ex = ioc.get_executor();
    constexpr std::uint16_t device_id = 1;

    auto [host_link, eq_link] = MemoryLink::create(ex);
    StateMachine host_sm(host_link, device_id);
    StateMachine eq_sm(eq_link, device_id);

    SessionOptions host_opt{};
    host_opt.t3 = 200ms;
    host_opt.poll_interval = 1ms;
    host_opt.secs1_reverse_bit = false;

    SessionOptions eq_opt = host_opt;
    eq_opt.secs1_reverse_bit = true;

    Session proto_host(host_sm, device_id, host_opt);
    Session proto_equip(eq_sm, device_id, eq_opt);

    // 1) 非空 body：回一个可解码的 Item
    proto_equip.router().set(
        1,
        1,
        [](const DataMessage &) -> asio::awaitable<HandlerResult> {
            co_return secs::utils::make_handler_result(Item::ascii("OK"));
        });

    // 2) 空 body：decoded 应为 nullopt
    proto_equip.router().set(
        1,
        3,
        [](const DataMessage &) -> asio::awaitable<HandlerResult> {
            co_return HandlerResult{std::error_code{}, {}};
        });

    // 3) 非法 body：触发 decode error
    proto_equip.router().set(
        1,
        5,
        [](const DataMessage &) -> asio::awaitable<HandlerResult> {
            std::vector<byte> bad = {static_cast<byte>(0xFF),
                                     static_cast<byte>(0xFF),
                                     static_cast<byte>(0xFF)};
            co_return HandlerResult{std::error_code{}, std::move(bad)};
        });

    asio::co_spawn(ex, proto_equip.async_run(), asio::detached);

    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            // 1) decoded 有值
            {
                auto [ec, out] = co_await secs::utils::async_request_decoded(
                    proto_host, 1, 1, bytes_view{}, 200ms);
                TEST_EXPECT_OK(ec);
                TEST_EXPECT_EQ(out.reply.stream, 1);
                TEST_EXPECT_EQ(out.reply.function, 2);
                TEST_EXPECT(out.decoded.has_value());
                TEST_EXPECT(out.decoded->fully_consumed);
                const auto *ascii = out.decoded->item.get_if<secs::ii::ASCII>();
                TEST_EXPECT(ascii != nullptr);
                TEST_EXPECT_EQ(ascii->value, std::string("OK"));
            }

            // 2) 空 body：decoded=nullopt
            {
                auto [ec, out] = co_await secs::utils::async_request_decoded(
                    proto_host, 1, 3, bytes_view{}, 200ms);
                TEST_EXPECT_OK(ec);
                TEST_EXPECT_EQ(out.reply.stream, 1);
                TEST_EXPECT_EQ(out.reply.function, 4);
                TEST_EXPECT(!out.decoded.has_value());
            }

            // 3) 非法 body：返回 decode error，同时保留 reply 便于排查
            {
                auto [ec, out] = co_await secs::utils::async_request_decoded(
                    proto_host, 1, 5, bytes_view{}, 200ms);
                TEST_EXPECT(static_cast<bool>(ec));
                TEST_EXPECT_EQ(out.reply.stream, 1);
                TEST_EXPECT_EQ(out.reply.function, 6);
                TEST_EXPECT(!out.decoded.has_value());
            }

            proto_host.stop();
            proto_equip.stop();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
}

} // namespace

int main() {
    test_ii_helpers_roundtrip();
    test_protocol_helpers_request_decoded();
    return secs::tests::run_and_report();
}
