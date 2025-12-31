#pragma once

#include "secs/core/common.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/message.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace secs::hsms {

struct ConnectionOptions final {
    // T8：网络字符间隔超时（字节间隔超时）。0 表示不启用。
    core::duration t8{};
};

/**
 * @brief HSMS 字节流抽象（用于在受限环境下进行纯内存/非 socket 的单元测试）。
 *
 * 说明：
 * - Connection 只依赖该接口的 read/write/cancel/close 语义（不关心底层是 TCP
 * 还是内存队列）。
 * - 生产环境默认使用 TCP 版本的 Stream；单元测试可注入纯内存 Stream。
 */
class Stream {
public:
    virtual ~Stream() = default;

    [[nodiscard]] virtual asio::any_io_executor executor() const noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    virtual void cancel() noexcept = 0;
    virtual void close() noexcept = 0;

    virtual asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(core::mutable_bytes_view dst) = 0;

    virtual asio::awaitable<std::error_code>
    async_write_all(core::bytes_view src) = 0;

    // 若底层不支持“连接”语义（例如纯内存流），这里可以直接返回
    // invalid_argument。
    virtual asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &endpoint) = 0;
};

/**
 * @brief HSMS 连接层：封装 TCP framing（Length + Header + Body）。
 *
 * 注意：
 * - 该类假设在同一 io_context/strand 语境中使用（与 secs::core::Event 一致）。
 * - 读操作建议由单协程驱动；写操作支持多协程并发调用并保证串行写入。
 */
class Connection final {
public:
    explicit Connection(asio::any_io_executor ex,
                        ConnectionOptions options = {});
    explicit Connection(asio::ip::tcp::socket socket,
                        ConnectionOptions options = {});
    explicit Connection(std::unique_ptr<Stream> stream,
                        ConnectionOptions options = {});

    [[nodiscard]] asio::any_io_executor executor() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &endpoint);
    asio::awaitable<std::error_code> async_close();

    void cancel_and_close() noexcept;

    asio::awaitable<std::error_code> async_write_message(const Message &msg);
    asio::awaitable<std::pair<std::error_code, Message>> async_read_message();

private:
    asio::awaitable<std::error_code>
    async_read_exactly(core::mutable_bytes_view dst);

    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some_with_t8(core::byte *dst, std::size_t n);

    static std::uint32_t read_u32_be_(const core::byte *p) noexcept;

    std::unique_ptr<Stream> stream_;
    ConnectionOptions options_{};

    // 写入串行化（避免多个协程并发发起 async_write 造成未定义行为）。
    secs::core::Event write_gate_{};
    bool write_in_progress_{false};
};

} // namespace secs::hsms
