/**
 * @file test_typed_handler.cpp
 * @brief TypedHandler 单元测试
 */

#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/typed_handler.hpp"

#include "test_main.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

using secs::ii::ASCII;
using secs::ii::Item;
using secs::ii::List;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using secs::protocol::register_typed_handler;
using secs::protocol::Router;
using secs::protocol::TypedHandler;

// ============================================================================
// 测试消息定义
// ============================================================================

struct TestRequest {
    std::string value;

    static std::optional<TestRequest> from_item(const secs::ii::Item &item) {
        auto *list = item.get_if<List>();
        if (!list || list->size() != 1) {
            return std::nullopt;
        }
        auto *ascii = (*list)[0].get_if<ASCII>();
        if (!ascii) {
            return std::nullopt;
        }
        return TestRequest{ascii->value};
    }

    secs::ii::Item to_item() const { return Item::list({Item::ascii(value)}); }
};

struct TestResponse {
    std::string result;

    static std::optional<TestResponse> from_item(const secs::ii::Item &item) {
        auto *list = item.get_if<List>();
        if (!list || list->size() != 1) {
            return std::nullopt;
        }
        auto *ascii = (*list)[0].get_if<ASCII>();
        if (!ascii) {
            return std::nullopt;
        }
        return TestResponse{ascii->value};
    }

    secs::ii::Item to_item() const { return Item::list({Item::ascii(result)}); }
};

// ============================================================================
// 测试 Handler 实现
// ============================================================================

class SuccessHandler : public TypedHandler<TestRequest, TestResponse> {
public:
    asio::awaitable<std::pair<std::error_code, TestResponse>>
    handle(const TestRequest &request,
           const DataMessage & /*原始消息*/) override {
        TestResponse response{"ECHO:" + request.value};
        co_return std::pair{std::error_code{}, response};
    }
};

class ErrorHandler : public TypedHandler<TestRequest, TestResponse> {
public:
    asio::awaitable<std::pair<std::error_code, TestResponse>>
    handle(const TestRequest & /*请求*/,
           const DataMessage & /*原始消息*/) override {
        co_return std::pair{make_error_code(errc::timeout), TestResponse{}};
    }
};

class NonStrictHandler : public TypedHandler<TestRequest, TestResponse> {
public:
    using Base = TypedHandler<TestRequest, TestResponse>;
    using DecodeOptions = typename Base::DecodeOptions;

    NonStrictHandler() : Base(DecodeOptions{.strict_consumed = false}) {}

    asio::awaitable<std::pair<std::error_code, TestResponse>>
    handle(const TestRequest &request,
           const DataMessage & /*原始消息*/) override {
        TestResponse response{"ECHO:" + request.value};
        co_return std::pair{std::error_code{}, response};
    }
};

class SmallLimitHandler : public TypedHandler<TestRequest, TestResponse> {
public:
    using Base = TypedHandler<TestRequest, TestResponse>;
    using DecodeOptions = typename Base::DecodeOptions;

    SmallLimitHandler(std::uint32_t max_payload_bytes)
        : Base(DecodeOptions{
              .limits = secs::ii::DecodeLimits{.max_payload_bytes = max_payload_bytes},
              .strict_consumed = true,
          }) {}

    asio::awaitable<std::pair<std::error_code, TestResponse>>
    handle(const TestRequest &request,
           const DataMessage & /*原始消息*/) override {
        TestResponse response{"ECHO:" + request.value};
        co_return std::pair{std::error_code{}, response};
    }
};

// ============================================================================
// 辅助函数
// ============================================================================

std::vector<byte> encode_request(const TestRequest &req) {
    auto item = req.to_item();
    std::vector<byte> body;
    secs::ii::encode(item, body);
    return body;
}

DataMessage make_data_message(const std::vector<byte> &body) {
    DataMessage msg;
    msg.stream = 1;
    msg.function = 1;
    msg.w_bit = true;
    msg.system_bytes = 12345;
    msg.body = body;
    return msg;
}

// ============================================================================
// 测试用例
// ============================================================================

void test_successful_handler() {
    asio::io_context ioc;
    auto handler = std::make_shared<SuccessHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TestRequest req{"hello"};
            auto body = encode_request(req);
            auto msg = make_data_message(body);

            auto [ec, response_body] = co_await handler->invoke(msg);

            TEST_EXPECT(!ec);
            TEST_EXPECT(!response_body.empty());

            // 解码响应
            Item response_item{List{}};
            std::size_t consumed = 0;
            auto decode_ec = secs::ii::decode_one(
                bytes_view{response_body.data(), response_body.size()},
                response_item,
                consumed);
            TEST_EXPECT(!decode_ec);

            auto response = TestResponse::from_item(response_item);
            TEST_EXPECT(response.has_value());
            TEST_EXPECT_EQ(response->result, std::string("ECHO:hello"));

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_error_empty_body() {
    asio::io_context ioc;
    auto handler = std::make_shared<SuccessHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            DataMessage msg;
            msg.stream = 1;
            msg.function = 1;
            msg.body = {}; // 空消息体

            auto [ec, response_body] = co_await handler->invoke(msg);

            TEST_EXPECT(ec == make_error_code(errc::invalid_argument));
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_error_invalid_item() {
    asio::io_context ioc;
    auto handler = std::make_shared<SuccessHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 编码一个不符合 TestRequest 结构的 Item (空 List)
            auto item = Item::list({});
            std::vector<byte> body;
            secs::ii::encode(item, body);

            auto msg = make_data_message(body);
            auto [ec, response_body] = co_await handler->invoke(msg);

            // from_item 返回 std::nullopt -> invalid_argument
            TEST_EXPECT(ec == make_error_code(errc::invalid_argument));
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_error_truncated_body() {
    asio::io_context ioc;
    auto handler = std::make_shared<SuccessHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 截断的消息体（只有部分头部）
            std::vector<byte> body = {0x01, 0x02}; // 无效的 SECS-II 数据

            auto msg = make_data_message(body);
            auto [ec, response_body] = co_await handler->invoke(msg);

            // decode_one 失败
            TEST_EXPECT(ec != std::error_code{});
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_handler_error() {
    asio::io_context ioc;
    auto handler = std::make_shared<ErrorHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TestRequest req{"test"};
            auto body = encode_request(req);
            auto msg = make_data_message(body);

            auto [ec, response_body] = co_await handler->invoke(msg);

            TEST_EXPECT(ec == make_error_code(errc::timeout));
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_strict_consumed_rejects_trailing_bytes() {
    asio::io_context ioc;
    auto handler = std::make_shared<SuccessHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TestRequest req{"hello"};
            auto body = encode_request(req);
            body.push_back(static_cast<byte>(0x00)); // 尾随垃圾字节

            auto msg = make_data_message(body);
            auto [ec, response_body] = co_await handler->invoke(msg);

            TEST_EXPECT(ec == make_error_code(errc::invalid_argument));
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_non_strict_consumed_allows_trailing_bytes() {
    asio::io_context ioc;
    auto handler = std::make_shared<NonStrictHandler>();

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TestRequest req{"hello"};
            auto body = encode_request(req);
            body.push_back(static_cast<byte>(0x00)); // 尾随垃圾字节

            auto msg = make_data_message(body);
            auto [ec, response_body] = co_await handler->invoke(msg);

            TEST_EXPECT_OK(ec);
            TEST_EXPECT(!response_body.empty());
            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_decode_limits_can_reject_large_payload() {
    asio::io_context ioc;
    auto handler = std::make_shared<SmallLimitHandler>(1U); // max_payload_bytes=1

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // ASCII payload 长度为 2，大于限制，应触发 ii::errc::payload_too_large
            TestRequest req{"aa"};
            auto body = encode_request(req);
            auto msg = make_data_message(body);

            auto [ec, response_body] = co_await handler->invoke(msg);
            TEST_EXPECT(ec == secs::ii::make_error_code(secs::ii::errc::payload_too_large));
            TEST_EXPECT(response_body.empty());

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_register_typed_handler() {
    asio::io_context ioc;
    Router router;

    auto handler = std::make_shared<SuccessHandler>();
    register_typed_handler(router, 1, 1, handler);

    // 验证处理器已注册
    auto found = router.find(1, 1);
    TEST_EXPECT(found.has_value());

    // 通过 Router 调用
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TestRequest req{"via_router"};
            auto body = encode_request(req);
            auto msg = make_data_message(body);

            auto [ec, response_body] = co_await (*found)(msg);

            TEST_EXPECT(!ec);

            // 解码响应
            Item response_item{List{}};
            std::size_t consumed = 0;
            secs::ii::decode_one(
                bytes_view{response_body.data(), response_body.size()},
                response_item,
                consumed);

            auto response = TestResponse::from_item(response_item);
            TEST_EXPECT(response.has_value());
            TEST_EXPECT_EQ(response->result, std::string("ECHO:via_router"));

            co_return;
        },
        asio::detached);

    ioc.run();
}

void test_multiple_handlers() {
    Router router;

    auto handler1 = std::make_shared<SuccessHandler>();
    auto handler2 = std::make_shared<ErrorHandler>();

    register_typed_handler(router, 1, 1, handler1);
    register_typed_handler(router, 1, 3, handler2);

    TEST_EXPECT(router.find(1, 1).has_value());
    TEST_EXPECT(router.find(1, 3).has_value());
    TEST_EXPECT(!router.find(1, 5).has_value());
}

void test_secs_message_concept() {
    // 编译期验证概念约束（concept）
    static_assert(secs::protocol::SecsMessage<TestRequest>);
    static_assert(secs::protocol::SecsMessage<TestResponse>);
}

} // namespace

int main() {
    test_successful_handler();
    test_decode_error_empty_body();
    test_decode_error_invalid_item();
    test_decode_error_truncated_body();
    test_handler_error();
    test_decode_strict_consumed_rejects_trailing_bytes();
    test_decode_non_strict_consumed_allows_trailing_bytes();
    test_decode_limits_can_reject_large_payload();
    test_register_typed_handler();
    test_multiple_handlers();
    test_secs_message_concept();

    return secs::tests::run_and_report();
}
