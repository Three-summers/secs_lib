/**
 * @file hsms_pipe_client.cpp
 * @brief HSMS（pipe）客户端示例：通过 stdin/stdout 承载 HSMS 帧（二进制流），作为主动端发起请求并校验回显。
 *
 * 用法：
 *   ./hsms_pipe_client [device_id]
 *
 * 说明：
 * - 本示例不使用 TCP（受限沙箱常禁用 socket()），改用“全双工管道”承载 HSMS 帧：
 *   - stdin：读对端输出的 HSMS 帧
 *   - stdout：写出本端 HSMS 帧
 * - stdout 只能输出二进制 HSMS 帧；所有日志请输出到 stderr，避免污染字节流。
 * - 需要外部把 client/server 的 stdin/stdout 交叉连接（见 examples/README.md）。
 */

#include "secs/core/error.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#
#include <signal.h>
#include <unistd.h>
#
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#
namespace {
#
using secs::core::byte;
using secs::core::bytes_view;
using secs::core::mutable_bytes_view;
#
using namespace std::chrono_literals;
#
class PipeStream final : public secs::hsms::Stream {
public:
    PipeStream(asio::any_io_executor ex, int read_fd, int write_fd)
        : ex_(ex), in_(ex, read_fd), out_(ex, write_fd) {
        std::error_code ignored;
        in_.non_blocking(true, ignored);
        out_.non_blocking(true, ignored);
    }
#
    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override {
        return in_.is_open() && out_.is_open();
    }
#
    void cancel() noexcept override {
        std::error_code ignored;
        in_.cancel(ignored);
        out_.cancel(ignored);
    }
#
    void close() noexcept override {
        std::error_code ignored;
        in_.close(ignored);
        out_.close(ignored);
    }
#
    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(mutable_bytes_view dst) override {
        auto [ec, n] = co_await in_.async_read_some(
            asio::buffer(dst.data(), dst.size()),
            asio::as_tuple(asio::use_awaitable));
        co_return std::pair{ec, n};
    }
#
    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        std::size_t offset = 0;
        while (offset < src.size()) {
            auto [ec, n] = co_await asio::async_write(
                out_,
                asio::buffer(src.data() + offset, src.size() - offset),
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return ec;
            }
            if (n == 0) {
                co_return secs::core::make_error_code(
                    secs::core::errc::invalid_argument);
            }
            offset += n;
        }
        co_return std::error_code{};
    }
#
    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
#
private:
    asio::any_io_executor ex_{};
    asio::posix::stream_descriptor in_;
    asio::posix::stream_descriptor out_;
};
#
secs::ii::Item make_test_item() {
    return secs::ii::Item::list({
        secs::ii::Item::u4({123U}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({secs::ii::Item::u1({1U, 2U, 3U})}),
    });
}
#
asio::awaitable<int> run(std::uint16_t device_id) {
    auto ex = co_await asio::this_coro::executor;
#
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 2s;
    options.t6 = 2s;
    options.t7 = 2s;
    options.t8 = 2s;
    options.auto_reconnect = false;
#
    secs::hsms::Session session(ex, options);
#
    auto stream =
        std::make_unique<PipeStream>(ex, STDIN_FILENO, STDOUT_FILENO);
    secs::hsms::Connection conn(
        std::move(stream), secs::hsms::ConnectionOptions{.t8 = options.t8});
#
    if (auto ec = co_await session.async_open_active(std::move(conn)); ec) {
        std::cerr << "[pipe_client] 主动打开会话失败: " << ec.message() << "\n";
        co_return 2;
    }
#
    // 额外演示一次 LINKTEST（可帮助对端覆盖 LINKTEST.req/resp 逻辑）。
    if (auto ec = co_await session.async_linktest(); ec) {
        std::cerr << "[pipe_client] LINKTEST 失败: " << ec.message() << "\n";
        session.stop();
        co_return 3;
    }
#
    const auto item = make_test_item();
    std::vector<byte> body;
    if (auto enc_ec = secs::ii::encode(item, body); enc_ec) {
        std::cerr << "[pipe_client] SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session.stop();
        co_return 4;
    }
#
    auto [req_ec, rsp] = co_await session.async_request_data(
        /*stream=*/1, /*function=*/13, bytes_view{body.data(), body.size()}, 2s);
    if (req_ec) {
        std::cerr << "[pipe_client] 请求-响应失败: " << req_ec.message() << "\n";
        session.stop();
        co_return 5;
    }
#
    if (rsp.stream() != 1 || rsp.function() != 14 || rsp.w_bit()) {
        std::cerr << "[pipe_client] 响应头不符合预期: stream="
                  << static_cast<int>(rsp.stream())
                  << " function=" << static_cast<int>(rsp.function())
                  << " w=" << (rsp.w_bit() ? 1 : 0) << "\n";
        session.stop();
        co_return 6;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[pipe_client] 响应 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        co_return 7;
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "[pipe_client] 响应 body 存在未消费尾部: consumed=" << consumed
                  << " total=" << rsp.body.size() << "\n";
        session.stop();
        co_return 8;
    }
    if (decoded != item) {
        std::cerr << "[pipe_client] 响应 Item 与请求不一致\n";
        session.stop();
        co_return 9;
    }
#
    std::cerr << "[pipe_client] PASS\n";
    session.stop();
    co_return 0;
}
#
} // namespace
#
int main(int argc, char **argv) {
    // pipe 场景：避免写入断管触发 SIGPIPE 终止进程（示例与单测都需要）。
    ::signal(SIGPIPE, SIG_IGN);
#
    std::uint16_t device_id = 1;
    if (argc >= 2) {
        device_id =
            static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
    }
#
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run(device_id);
            io.stop();
        },
        asio::detached);
    io.run();
    return rc;
}

