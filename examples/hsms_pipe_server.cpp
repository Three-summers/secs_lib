/**
 * @file hsms_pipe_server.cpp
 * @brief HSMS（pipe）服务端示例：通过 stdin/stdout 承载 HSMS 帧（二进制流），作为被动端回显请求。
 *
 * 用法：
 *   ./hsms_pipe_server [device_id]
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
#include "secs/ii/item.hpp"
#include "secs/utils/ii_helpers.hpp"
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
secs::ii::Item make_expected_item() {
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
    if (auto ec = co_await session.async_open_passive(std::move(conn)); ec) {
        std::cerr << "[pipe_server] 打开被动会话失败: " << ec.message() << "\n";
        co_return 2;
    }
#
    auto [rec_ec, req] = co_await session.async_receive_data(2s);
    if (rec_ec) {
        std::cerr << "[pipe_server] 接收 data message 失败: " << rec_ec.message()
                  << "\n";
        session.stop();
        co_return 3;
    }
#
    // 验证请求体（示例用固定 Item；不一致也可继续回显，但单测会要求一致）。
    const auto expected = make_expected_item();
    auto [dec_ec, decoded] = secs::utils::decode_one_item(
        bytes_view{req.body.data(), req.body.size()});
    if (dec_ec) {
        std::cerr << "[pipe_server] 请求 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        co_return 4;
    }
    if (!decoded.fully_consumed) {
        std::cerr << "[pipe_server] 请求 body 存在未消费尾部: consumed="
                  << decoded.consumed
                  << " total=" << req.body.size() << "\n";
        session.stop();
        co_return 5;
    }
    if (decoded.item != expected) {
        std::cerr << "[pipe_server] 请求 Item 与预期不一致\n";
        session.stop();
        co_return 6;
    }
#
    // 回包：SxF(y+1)，system_bytes 必须回显以匹配对端挂起请求。
    const auto rsp = secs::hsms::make_data_message(
        req.header.session_id,
        req.stream(),
        static_cast<std::uint8_t>(req.function() + 1U),
        false,
        req.header.system_bytes,
        bytes_view{req.body.data(), req.body.size()});
#
    if (auto send_ec = co_await session.async_send(rsp); send_ec) {
        std::cerr << "[pipe_server] 发送响应失败: " << send_ec.message() << "\n";
        session.stop();
        co_return 7;
    }
#
    std::cerr << "[pipe_server] PASS\n";
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
