#pragma once

#include "secs/core/common.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/message.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>

#include <deque>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

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

    // data 写门禁：
    // - NOT_SELECTED 状态下可禁用 data 写入，避免控制流期间出现“data 抢写”竞态。
    // - 禁用时会快速失败队列中尚未写出的 data。
    void enable_data_writes() noexcept;
    void disable_data_writes(std::error_code reason) noexcept;

    asio::awaitable<std::error_code> async_write_message(const Message &msg);
    asio::awaitable<std::pair<std::error_code, Message>> async_read_message();

private:
    struct WriteRequest final {
        std::vector<core::byte> frame{};
        secs::core::Event done{};
        std::error_code ec{};
        bool is_data{false};
    };

    asio::awaitable<std::error_code>
    async_read_exactly(core::mutable_bytes_view dst);

    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some_with_t8(core::byte *dst, std::size_t n);

    static std::uint32_t read_u32_be_(const core::byte *p) noexcept;

    void start_writer_();
    asio::awaitable<void> writer_loop_();
    void cancel_queued_writes_(std::error_code reason) noexcept;
    void cancel_queued_data_writes_(std::error_code reason) noexcept;

    std::unique_ptr<Stream> stream_;
    ConnectionOptions options_{};

    // 写入串行化 + 控制消息优先级：
    // - 统一由 writer_loop_ 串行写出，避免并发 async_write 未定义行为
    // - control_queue_ 优先于 data_queue_，避免 Deselect/Separate 等控制消息被 data
    //   抢占写入顺序
    secs::core::Event write_ready_{};
    std::deque<std::shared_ptr<WriteRequest>> control_queue_{};
    std::deque<std::shared_ptr<WriteRequest>> data_queue_{};
    bool writer_running_{false};
    bool data_writes_enabled_{true};
};

} // namespace secs::hsms
