#include "secs/hsms/session.hpp"
#include "secs/protocol/session.hpp"
#
#include "secs/core/error.hpp"
#
#include "test_main.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#
#include <signal.h>
#
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#
namespace {
#
using namespace std::chrono_literals;
#
using secs::core::errc;
using secs::core::make_error_code;
#
static void test_protocol_session_destructor_is_safe_with_detached_run_loop() {
    asio::io_context ioc;
    std::atomic<bool> done{false};
#
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ex = co_await asio::this_coro::executor;
#
            secs::hsms::SessionOptions hsms_opt{};
            hsms_opt.session_id = 1;
            hsms_opt.t3 = 100ms;
            hsms_opt.t6 = 100ms;
            hsms_opt.t7 = 100ms;
            hsms_opt.t8 = 50ms;
            hsms_opt.auto_reconnect = false;
#
            secs::hsms::Session hsms(ex, hsms_opt);
#
            secs::protocol::SessionOptions proto_opt{};
            proto_opt.t3 = 100ms;
            proto_opt.poll_interval = 1ms;
#
            // 关键路径：
            // - async_request(HSMS) 会自动启动一个 detached 的 async_run_impl_；
            // - 若该 run loop 捕获裸 this，当 Session 析构后仍可能在 HSMS stop 触发后恢复并崩溃。
            {
                auto proto = std::make_unique<secs::protocol::Session>(
                    hsms,
                    /*session_id=*/hsms_opt.session_id,
                    proto_opt);
#
                std::array<secs::core::byte, 1> body{{0x01}};
                auto [ec, _] = co_await proto->async_request(
                    /*stream=*/1,
                    /*function=*/1,
                    secs::core::bytes_view{body.data(), body.size()},
                    100ms);
                TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
#
                // 给 detached run loop 一个机会进入“等待收包”态。
                asio::steady_timer t(ex);
                t.expires_after(10ms);
                (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
#
                // 不显式 stop()，直接析构 protocol::Session。
                proto.reset();
            }
#
            // 触发 HSMS stop：若旧实现存在 UAF，这里会高概率崩溃。
            hsms.stop();
#
            asio::steady_timer t2(ex);
            t2.expires_after(100ms);
            (void)co_await t2.async_wait(asio::as_tuple(asio::use_awaitable));
#
            done = true;
            ioc.stop();
            co_return;
        },
        asio::detached);
#
    ioc.run();
    TEST_EXPECT(done.load());
}
#
} // namespace
#
int main() {
    ::signal(SIGPIPE, SIG_IGN);
#
    test_protocol_session_destructor_is_safe_with_detached_run_loop();
#
    return ::secs::tests::run_and_report();
}
