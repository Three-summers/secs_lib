#pragma once

#include "secs/core/common.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace secs::hsms {

enum class SessionState : std::uint8_t {
    disconnected = 0,
    connected = 1,
    selected = 2,
};

struct SessionOptions final {
    // HSMS-SS：data message 的 SessionID（Device ID，低 15 位有效）。
    // 控制消息（SELECT/LINKTEST/SEPARATE 等）SessionID 固定为 0xFFFF。
    std::uint16_t session_id{0};

    // HSMS 定时器（默认值偏向测试可控，生产使用建议由上层显式配置）。
    core::duration t3{std::chrono::seconds{45}}; // T3：回复超时
    core::duration t5{
        std::chrono::seconds{10}}; // T5：重连延迟 / SEPARATE 后退避延迟
    core::duration t6{std::chrono::seconds{
        5}}; // T6：控制事务超时（SELECT/DESELECT/LINKTEST 等）
    core::duration t7{
        std::chrono::seconds{10}}; // T7：未 selected 超时（被动端等待 SELECT）
    core::duration t8{std::chrono::seconds{5}}; // T8：网络字符间隔超时

    // 链路测试（LINKTEST）周期（0 表示不自动发送）。
    core::duration linktest_interval{};
    // Linktest 连续失败阈值：达到阈值后断线（默认 1：一次失败即断线，保持当前行为）。
    std::uint32_t linktest_max_consecutive_failures{1};

    bool auto_reconnect{true};

    // 被动端是否接受 SELECT（用于单测覆盖“拒绝”分支）。
    bool passive_accept_select{true};
};

/**
 * @brief HSMS-SS 会话：连接管理 + 选择状态机 + 控制消息 + 定时器。
 *
 * 设计目标：
 * - 连接层（Connection）只做分帧（framing）与读写；Session
 * 做协议控制流与定时器策略。
 * - 不引入额外依赖，错误通过 std::error_code 返回。
 */
class Session final {
public:
    explicit Session(asio::any_io_executor ex, SessionOptions options);

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return executor_;
    }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] bool is_selected() const noexcept {
        return state_ == SessionState::selected;
    }

    [[nodiscard]] std::uint64_t selected_generation() const noexcept {
        return selected_generation_.load();
    }
    [[nodiscard]] std::uint32_t allocate_system_bytes() noexcept {
        return system_bytes_.fetch_add(1U);
    }

    void stop() noexcept;

    asio::awaitable<std::error_code>
    async_open_active(const asio::ip::tcp::endpoint &endpoint);
    asio::awaitable<std::error_code> async_open_active(Connection &&connection);
    asio::awaitable<std::error_code>
    async_open_passive(asio::ip::tcp::socket socket);
    asio::awaitable<std::error_code>
    async_open_passive(Connection &&connection);

    // 主动端自动重连主循环：直到 stop()，或 auto_reconnect==false 且发生断线。
    asio::awaitable<std::error_code>
    async_run_active(const asio::ip::tcp::endpoint &endpoint);

    asio::awaitable<std::error_code> async_send(const Message &msg);

    // 等待下一条数据消息（控制消息会被内部消费/响应）。
    asio::awaitable<std::pair<std::error_code, Message>>
    async_receive_data(std::optional<core::duration> timeout = std::nullopt);

    // 发送数据主消息（W=1），并等待同 SystemBytes 的数据消息作为回应（T3）。
    asio::awaitable<std::pair<std::error_code, Message>> async_request_data(
        std::uint8_t stream, std::uint8_t function, core::bytes_view body);

    // 发送数据主消息（W=1），并等待同 SystemBytes 的数据消息作为回应（timeout
    // 优先，否则使用默认 T3）。
    asio::awaitable<std::pair<std::error_code, Message>>
    async_request_data(std::uint8_t stream,
                       std::uint8_t function,
                       core::bytes_view body,
                       std::optional<core::duration> timeout);

    // 显式发起 LINKTEST（T6）。
    asio::awaitable<std::error_code> async_linktest();

    // 等待会话进入已选择（selected）状态；min_generation
    // 用于等待“下一次重连后”的 selected。
    asio::awaitable<std::error_code>
    async_wait_selected(std::uint64_t min_generation, core::duration timeout);

    // 等待内部 reader_loop_ 退出（用于资源安全回收；timeout=空表示无限等待）。
    asio::awaitable<std::error_code> async_wait_reader_stopped(
        std::optional<core::duration> timeout = std::nullopt);

private:
    struct Pending final {
        explicit Pending(SType expected) : expected_stype(expected) {}

        SType expected_stype;
        secs::core::Event ready{};
        std::error_code ec{};
        std::optional<Message> response{};
    };

    void reset_state_() noexcept;
    void set_selected_() noexcept;
    void set_not_selected_() noexcept;
    void on_disconnected_(std::error_code reason) noexcept;

    void start_reader_();
    asio::awaitable<void> reader_loop_();
    asio::awaitable<void> linktest_loop_(std::uint64_t generation);

    asio::awaitable<std::pair<std::error_code, Message>>
    async_control_transaction_(const Message &req,
                               SType expected_rsp,
                               core::duration timeout);

    asio::awaitable<std::pair<std::error_code, Message>>
    async_data_transaction_(const Message &req, core::duration timeout);

    [[nodiscard]] bool fulfill_pending_(Message &msg) noexcept;
    void cancel_pending_data_(std::error_code reason) noexcept;

    asio::any_io_executor executor_;
    SessionOptions options_{};

    Connection connection_;

    std::atomic<std::uint32_t> system_bytes_{1};

    SessionState state_{SessionState::disconnected};
    std::atomic<std::uint64_t> selected_generation_{0};

    bool stop_requested_{false};
    bool reader_running_{false};

    secs::core::Event selected_event_{};
    secs::core::Event disconnected_event_{};
    secs::core::Event reader_stopped_event_{};

    std::deque<Message> inbound_data_{};
    secs::core::Event inbound_event_{};

    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_{};
};

} // namespace secs::hsms
