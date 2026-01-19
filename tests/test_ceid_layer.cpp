#include "test_main.hpp"

#include <secs/core/common.hpp>
#include <secs/core/error.hpp>
#include <secs/ii/item.hpp>
#include <secs/protocol/ceid_dispatcher.hpp>
#include <secs/protocol/session.hpp>
#include <secs/secs1/link.hpp>
#include <secs/secs1/state_machine.hpp>
#include <secs/utils/ceid_helpers.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;

using secs::ii::ASCII;
using secs::ii::Item;
using secs::ii::List;

using secs::protocol::CeidDispatcher;
using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using secs::protocol::Session;
using secs::protocol::SessionOptions;

using secs::secs1::MemoryLink;
using secs::secs1::StateMachine;

void test_ceid_extractors() {
    {
        const Item body =
            Item::list({Item::u4({1}), Item::u2({100}), Item::list({})});
        const auto ceid = secs::utils::extract_ceid_s6f11_like(body);
        TEST_EXPECT(ceid.has_value());
        TEST_EXPECT_EQ(ceid.value(), 100U);
    }

    {
        const Item body =
            Item::list({Item::u4({1}),
                        Item::u8({static_cast<std::uint64_t>(
                            std::numeric_limits<std::uint32_t>::max()) +
                                  1U}),
                        Item::list({})});
        const auto ceid = secs::utils::extract_ceid_s6f11_like(body);
        TEST_EXPECT(!ceid.has_value());
    }
}

void test_ceid_helpers_extract_unsigned_scalar_u32_and_list_at() {
    using secs::utils::extract_list_unsigned_u32_at;
    using secs::utils::extract_unsigned_scalar_u32;

    // extract_unsigned_scalar_u32：支持 U1/U2/U4/U8（单值），其他情况返回 nullopt。
    TEST_EXPECT_EQ(extract_unsigned_scalar_u32(Item::u1({7})).value(), 7U);
    TEST_EXPECT(!extract_unsigned_scalar_u32(Item::u1({1, 2})).has_value());

    TEST_EXPECT_EQ(extract_unsigned_scalar_u32(Item::u2({300})).value(), 300U);
    TEST_EXPECT(!extract_unsigned_scalar_u32(Item::u2({1, 2})).has_value());

    TEST_EXPECT_EQ(extract_unsigned_scalar_u32(Item::u4({400})).value(), 400U);
    TEST_EXPECT(!extract_unsigned_scalar_u32(Item::u4({1, 2})).has_value());

    TEST_EXPECT_EQ(extract_unsigned_scalar_u32(Item::u8({500})).value(), 500U);
    TEST_EXPECT(!extract_unsigned_scalar_u32(Item::u8({1, 2})).has_value());

    TEST_EXPECT(!extract_unsigned_scalar_u32(Item::ascii("x")).has_value());

    // extract_list_unsigned_u32_at：对非 List 或越界返回 nullopt。
    const Item list = Item::list({Item::u1({1}), Item::u4({100})});
    TEST_EXPECT_EQ(extract_list_unsigned_u32_at(list, 0).value(), 1U);
    TEST_EXPECT_EQ(extract_list_unsigned_u32_at(list, 1).value(), 100U);
    TEST_EXPECT(!extract_list_unsigned_u32_at(list, 2).has_value());
    TEST_EXPECT(!extract_list_unsigned_u32_at(Item::ascii("x"), 0).has_value());
}

void test_ceid_dispatcher_routes_and_replies() {
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

    auto extractor = [](const DataMessage &, const Item &body)
        -> std::optional<CeidDispatcher::Ceid> {
        const auto ceid = secs::utils::extract_ceid_s6f11_like(body);
        if (!ceid.has_value()) {
            return std::nullopt;
        }
        return static_cast<CeidDispatcher::Ceid>(ceid.value());
    };

    auto dispatcher = std::make_shared<CeidDispatcher>(extractor);
    dispatcher->set_default_item(
        [](CeidDispatcher::Ceid ceid,
           const Item &,
           const DataMessage &) -> asio::awaitable<CeidDispatcher::ItemHandlerResult> {
            co_return std::pair{std::error_code{},
                                Item::list({Item::u4({1}),
                                            Item::u4({ceid}),
                                            Item::ascii("DEFAULT")})};
        });
    dispatcher->set_item(
        100,
        [](CeidDispatcher::Ceid ceid,
           const Item &,
           const DataMessage &) -> asio::awaitable<CeidDispatcher::ItemHandlerResult> {
            co_return std::pair{std::error_code{},
                                Item::list({Item::u4({1}),
                                            Item::u4({ceid}),
                                            Item::ascii("ACK")})};
        });

    secs::protocol::register_ceid_dispatcher(
        proto_equip.router(), 6, 11, dispatcher);

    asio::co_spawn(ex, proto_equip.async_run(), asio::detached);

    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            // 1) 命中 CEID=100 的精确处理器
            {
                const Item request = Item::list(
                    {Item::u4({1}), Item::u4({100}), Item::list({})});
                auto [ec, out] = co_await secs::utils::async_request_decoded_with_ceid(
                    proto_host,
                    6,
                    11,
                    request,
                    secs::utils::extract_ceid_s6f11_like,
                    secs::utils::extract_ceid_s6f11_like,
                    true,
                    200ms);

                TEST_EXPECT_OK(ec);
                TEST_EXPECT_EQ(out.reply.stream, 6);
                TEST_EXPECT_EQ(out.reply.function, 12);
                TEST_EXPECT(out.decoded.has_value());
                TEST_EXPECT(out.request_ceid.has_value());
                TEST_EXPECT(out.reply_ceid.has_value());
                TEST_EXPECT_EQ(out.request_ceid.value(), 100U);
                TEST_EXPECT_EQ(out.reply_ceid.value(), 100U);

                auto *list = out.decoded->item.get_if<List>();
                TEST_EXPECT(list != nullptr);
                TEST_EXPECT(list->size() >= 3U);
                auto *ascii = (*list)[2].get_if<ASCII>();
                TEST_EXPECT(ascii != nullptr);
                TEST_EXPECT_EQ(ascii->value, std::string("ACK"));
            }

            // 2) 未注册的 CEID：走 default
            {
                const Item request = Item::list(
                    {Item::u4({1}), Item::u4({123}), Item::list({})});
                auto [ec, out] = co_await secs::utils::async_request_decoded_with_ceid(
                    proto_host,
                    6,
                    11,
                    request,
                    secs::utils::extract_ceid_s6f11_like,
                    secs::utils::extract_ceid_s6f11_like,
                    true,
                    200ms);

                TEST_EXPECT_OK(ec);
                TEST_EXPECT(out.decoded.has_value());
                auto *list = out.decoded->item.get_if<List>();
                TEST_EXPECT(list != nullptr);
                auto *ascii = (*list)[2].get_if<ASCII>();
                TEST_EXPECT(ascii != nullptr);
                TEST_EXPECT_EQ(ascii->value, std::string("DEFAULT"));
            }

            proto_host.stop();
            proto_equip.stop();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
}

void test_ceid_dispatcher_missing_handler_returns_invalid_argument() {
    asio::io_context ioc;
    const auto ex = ioc.get_executor();

    auto extractor = [](const DataMessage &, const Item &body)
        -> std::optional<CeidDispatcher::Ceid> {
        const auto ceid = secs::utils::extract_ceid_s6f11_like(body);
        if (!ceid.has_value()) {
            return std::nullopt;
        }
        return static_cast<CeidDispatcher::Ceid>(ceid.value());
    };

    // 未注册任何 handler，也不设置 default：invoke() 应在 find_() 返回 nullopt 时失败。
    auto dispatcher = std::make_shared<CeidDispatcher>(extractor);

    const Item request =
        Item::list({Item::u4({1}), Item::u4({100}), Item::list({})});
    auto [enc_ec, bytes] = secs::utils::encode_item(request);
    TEST_EXPECT_OK(enc_ec);

    DataMessage msg{};
    msg.stream = 6;
    msg.function = 11;
    msg.w_bit = true;
    msg.system_bytes = 0;
    msg.body = std::move(bytes);

    std::optional<std::error_code> out_ec;
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto [ec, body] = co_await dispatcher->invoke(msg);
            out_ec = ec;
            TEST_EXPECT(body.empty());
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(out_ec.has_value());
    if (out_ec.has_value()) {
        TEST_EXPECT_EQ(*out_ec,
                       secs::core::make_error_code(secs::core::errc::invalid_argument));
    }
}

void test_async_request_decoded_with_ceid_mismatch() {
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

    // 回复 CEID 故意 +1，触发 verify_equal 失败
    proto_equip.router().set(
        6,
        11,
        [](const DataMessage &) -> asio::awaitable<HandlerResult> {
            co_return secs::utils::make_handler_result(
                Item::list({Item::u4({1}), Item::u4({101}), Item::ascii("NG")}));
        });

    asio::co_spawn(ex, proto_equip.async_run(), asio::detached);

    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            const Item request =
                Item::list({Item::u4({1}), Item::u4({100}), Item::list({})});
            auto [ec, out] = co_await secs::utils::async_request_decoded_with_ceid(
                proto_host,
                6,
                11,
                request,
                secs::utils::extract_ceid_s6f11_like,
                secs::utils::extract_ceid_s6f11_like,
                true,
                200ms);

            TEST_EXPECT_EQ(
                ec,
                secs::core::make_error_code(secs::core::errc::invalid_argument));
            TEST_EXPECT_EQ(out.reply.stream, 6);
            TEST_EXPECT_EQ(out.reply.function, 12);
            TEST_EXPECT(out.request_ceid.has_value());
            TEST_EXPECT(out.reply_ceid.has_value());
            TEST_EXPECT_EQ(out.request_ceid.value(), 100U);
            TEST_EXPECT_EQ(out.reply_ceid.value(), 101U);

            proto_host.stop();
            proto_equip.stop();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
}

} // namespace

int main() {
    test_ceid_extractors();
    test_ceid_helpers_extract_unsigned_scalar_u32_and_list_at();
    test_ceid_dispatcher_routes_and_replies();
    test_ceid_dispatcher_missing_handler_returns_invalid_argument();
    test_async_request_decoded_with_ceid_mismatch();
    return secs::tests::run_and_report();
}
