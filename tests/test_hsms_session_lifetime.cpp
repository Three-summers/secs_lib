#include "secs/hsms/connection.hpp"
#include "secs/hsms/session.hpp"
#
#include "secs/core/error.hpp"
#
#include "test_main.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#
#include <signal.h>
#include <unistd.h>
#
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#
namespace {
#
using namespace std::chrono_literals;
#
using secs::core::errc;
using secs::core::make_error_code;
#
struct UniqueFd final {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int v) : fd(v) {}
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this == &o) {
            return *this;
        }
        reset();
        fd = o.fd;
        o.fd = -1;
        return *this;
    }
    ~UniqueFd() { reset(); }
#
    void reset() noexcept {
        if (fd >= 0) {
            (void)::close(fd);
        }
        fd = -1;
    }
#
    [[nodiscard]] int release() noexcept {
        const int v = fd;
        fd = -1;
        return v;
    }
};
#
class PipeStream final : public secs::hsms::Stream {
public:
    PipeStream(asio::any_io_executor ex, int read_fd, int write_fd)
        : ex_(ex), reader_(ex, read_fd), writer_(ex, write_fd) {}
#
    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
#
    void cancel() noexcept override {
        std::error_code ignored;
        reader_.cancel(ignored);
        writer_.cancel(ignored);
    }
#
    void close() noexcept override {
        if (!open_) {
            return;
        }
        open_ = false;
        std::error_code ignored;
        reader_.close(ignored);
        writer_.close(ignored);
    }
#
    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(secs::core::mutable_bytes_view dst) override {
        auto [ec, n] = co_await reader_.async_read_some(
            asio::buffer(dst.data(), dst.size()),
            asio::as_tuple(asio::use_awaitable));
        co_return std::pair{ec, n};
    }
#
    asio::awaitable<std::error_code>
    async_write_all(secs::core::bytes_view src) override {
        auto [ec, n] =
            co_await asio::async_write(writer_,
                                       asio::buffer(src.data(), src.size()),
                                       asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return ec;
        }
        if (n != src.size()) {
            co_return make_error_code(errc::invalid_argument);
        }
        co_return std::error_code{};
    }
#
    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }
#
private:
    asio::any_io_executor ex_;
    asio::posix::stream_descriptor reader_;
    asio::posix::stream_descriptor writer_;
    bool open_{true};
};
#
struct PipeDuplex final {
    std::unique_ptr<secs::hsms::Stream> client_stream;
    std::unique_ptr<secs::hsms::Stream> server_stream;
};
#
static std::pair<std::error_code, PipeDuplex>
make_pipe_duplex(asio::any_io_executor client_ex,
                 asio::any_io_executor server_ex) {
    int c2s[2]{-1, -1};
    int s2c[2]{-1, -1};
    if (::pipe(c2s) != 0) {
        return {std::error_code(errno, std::generic_category()), PipeDuplex{}};
    }
    if (::pipe(s2c) != 0) {
        (void)::close(c2s[0]);
        (void)::close(c2s[1]);
        return {std::error_code(errno, std::generic_category()), PipeDuplex{}};
    }
#
    // client: 读 s2c[0]，写 c2s[1]
    // server: 读 c2s[0]，写 s2c[1]
    UniqueFd c2s_r(c2s[0]);
    UniqueFd c2s_w(c2s[1]);
    UniqueFd s2c_r(s2c[0]);
    UniqueFd s2c_w(s2c[1]);
#
    PipeDuplex duplex;
    duplex.client_stream = std::make_unique<PipeStream>(
        client_ex, s2c_r.release(), c2s_w.release());
    duplex.server_stream = std::make_unique<PipeStream>(
        server_ex, c2s_r.release(), s2c_w.release());
#
    return {std::error_code{}, std::move(duplex)};
}
#
static secs::hsms::SessionOptions make_opts(std::uint16_t device_id) {
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 500ms;
    options.t6 = 500ms;
    options.t7 = 500ms;
    options.t8 = 200ms;
    options.auto_reconnect = false;
    return options;
}
#
static void test_session_destructor_is_safe_with_running_reader() {
    asio::io_context ioc;
    std::atomic<bool> done{false};
#
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ex = co_await asio::this_coro::executor;
#
            auto [duplex_ec, duplex] = make_pipe_duplex(ex, ex);
            TEST_EXPECT_OK(duplex_ec);
#
            const auto opts = make_opts(/*device_id=*/1);
            auto server = std::make_unique<secs::hsms::Session>(ex, opts);
            auto client = std::make_unique<secs::hsms::Session>(ex, opts);
#
            secs::hsms::Connection server_conn(
                std::move(duplex.server_stream),
                secs::hsms::ConnectionOptions{.t8 = opts.t8});
            secs::hsms::Connection client_conn(
                std::move(duplex.client_stream),
                secs::hsms::ConnectionOptions{.t8 = opts.t8});
#
            secs::core::Event server_opened{};
            secs::core::Event client_opened{};
#
            asio::co_spawn(
                ex,
                [s = server.get(), conn = std::move(server_conn), &server_opened]() mutable
                -> asio::awaitable<void> {
                    auto ec = co_await s->async_open_passive(std::move(conn));
                    TEST_EXPECT_OK(ec);
                    server_opened.set();
                    co_return;
                },
                asio::detached);
#
            asio::co_spawn(
                ex,
                [c = client.get(), conn = std::move(client_conn), &client_opened]() mutable
                -> asio::awaitable<void> {
                    auto ec = co_await c->async_open_active(std::move(conn));
                    TEST_EXPECT_OK(ec);
                    client_opened.set();
                    co_return;
                },
                asio::detached);
#
            TEST_EXPECT_OK(co_await server_opened.async_wait(2s));
            TEST_EXPECT_OK(co_await client_opened.async_wait(2s));
#
            // 关键：不显式 stop()/wait_reader_stopped()，直接析构 Session。
            // 若内部 reader_loop_ 捕获裸 this 并在析构后继续运行，这里会高概率崩溃。
            server.reset();
            client.reset();
#
            asio::steady_timer t(ex);
            t.expires_after(200ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
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
    test_session_destructor_is_safe_with_running_reader();
#
    return ::secs::tests::run_and_report();
}
