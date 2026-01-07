#include "secs/core/error.hpp"
#include "secs/core/event.hpp"

#include "test_main.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <chrono>

namespace {

using secs::core::errc;
using secs::core::Event;
using secs::core::make_error_code;

using namespace std::chrono_literals;

void test_set_then_wait_immediate() {
    asio::io_context ioc;
    Event ev;
    ev.set();

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(10ms);
            TEST_EXPECT_OK(ec);
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_timeout() {
    asio::io_context ioc;
    Event ev;

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(5ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_cancel() {
    asio::io_context ioc;
    Event ev;

    asio::steady_timer t(ioc);
    t.expires_after(1ms);
    t.async_wait([&](const std::error_code &) { ev.cancel(); });

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(200ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_multi_waiters() {
    asio::io_context ioc;
    Event ev;

    constexpr int kN = 5;
    std::atomic<int> woke{0};

    asio::steady_timer set_timer(ioc);
    set_timer.expires_after(1ms);
    set_timer.async_wait([&](const std::error_code &) { ev.set(); });

    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired (possible deadlock)");
        ioc.stop();
    });

    for (int i = 0; i < kN; ++i) {
        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                auto ec = co_await ev.async_wait(200ms);
                TEST_EXPECT_OK(ec);
                if (++woke == kN) {
                    ioc.stop();
                }
                co_return;
            },
            asio::detached);
    }

    ioc.run();
    TEST_EXPECT_EQ(woke.load(), kN);
}

void test_reset() {
    asio::io_context ioc;
    Event ev;

    ev.set();
    ev.reset();

    std::atomic<bool> timed_out{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(5ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            timed_out = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(timed_out.load());
}

void test_max_waiters_limit() {
    asio::io_context ioc;

    // max_waiters=1：第 2 个等待者应快速失败（out_of_memory）。
    Event ev(/*max_waiters=*/1);

    // 先启动第 1 个等待者，并用 poll() 让其跑到“真正挂起”的位置。
    std::atomic<bool> waiter1_done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(200ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            waiter1_done = true;
            co_return;
        },
        asio::detached);

    // 驱动一次事件循环，确保 waiter1 已把自己加入 waiters_ 并挂起。
    (void)ioc.poll();

    std::atomic<bool> waiter2_done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await ev.async_wait(200ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::out_of_memory));
            waiter2_done = true;
            co_return;
        },
        asio::detached);

    asio::steady_timer cancel_timer(ioc);
    cancel_timer.expires_after(1ms);
    cancel_timer.async_wait([&](const std::error_code &) { ev.cancel(); });

    ioc.run();
    TEST_EXPECT(waiter1_done.load());
    TEST_EXPECT(waiter2_done.load());
}

} // namespace

int main() {
    test_set_then_wait_immediate();
    test_timeout();
    test_cancel();
    test_multi_waiters();
    test_reset();
    test_max_waiters_limit();
    return ::secs::tests::run_and_report();
}
