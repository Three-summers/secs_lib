#pragma once

#include "secs/core/common.hpp"
#include "secs/core/event.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/system_bytes.hpp"
#include "secs/utils/hsms_dump.hpp"
#include "secs/utils/secs1_dump.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace secs::hsms {
class Session;
} // namespace secs::hsms

namespace secs::secs1 {
class StateMachine;
} // namespace secs::secs1

namespace secs::protocol {

struct SessionOptions final {
    // T3：回复超时（协议层请求-响应匹配用）。
    secs::core::duration t3{std::chrono::seconds{45}};

    // HSMS 后端挂起请求上限（system_bytes -> Pending）。
    // 达到上限时，async_request(HSMS) 会快速失败，避免 pending_ 无界增长。
    std::size_t max_pending_requests{256};

    // 接收循环的轮询间隔：用于 stop() 检查与避免永久阻塞。
    secs::core::duration poll_interval{std::chrono::milliseconds{10}};

    // 仅对 SECS-I 后端有效：R-bit（reverse_bit）方向位。
    // - false：Host -> Equipment（R=0）
    // - true：Equipment -> Host（R=1）
    bool secs1_reverse_bit{false};

    /**
     * @brief 运行时报文 dump（调试用途）。
     *
     * 说明：
     * - 默认关闭，避免产生额外开销与日志噪声；
     * - 开启后会在 protocol 层对每条“发送/接收”的 DataMessage 进行解析并输出；
     * - HSMS 后端输出基于 `secs::utils::dump_hsms_frame`（会额外 encode 一次，仅用于 dump）；
     * - SECS-I 后端输出基于 `secs::utils::dump_secs1_message`（消息级别，不含 ENQ/EOT/ACK/NAK）。
     */
    struct DumpOptions final {
        // 总开关
        bool enable{false};

        // 方向开关
        bool dump_tx{true};
        bool dump_rx{true};

        // 输出 sink：
        // - 若为 nullptr：使用库内 spdlog 输出（INFO 级别）；
        // - 若非空：回调接收完整字符串（可能包含多行与 ANSI 颜色码）。
        using SinkFn =
            void (*)(void *user, const char *data, std::size_t size) noexcept;
        SinkFn sink{nullptr};
        void *sink_user{nullptr};

        // HSMS dump 选项（backend=HSMS 时生效）
        secs::utils::HsmsDumpOptions hsms{};

        // SECS-I dump 选项（backend=SECS-I 时生效）
        secs::utils::Secs1DumpOptions secs1{};
    };

    DumpOptions dump{};
};

/**
 * @brief 协议层会话：统一 HSMS 与 SECS-I 的“发送/请求/接收循环”接口。
 *
 * 能力：
 * - SystemBytes 分配与追踪（释放重用、回绕）
 * - 基于 SystemBytes + (Stream/Function) 的请求-响应匹配（T3）
 * - 根据 (Stream, Function) 路由入站主消息到注册的处理器，并在 W 位=1
 * 时自动回从消息
 *
 * 说明：
 * - HSMS（全双工）推荐同时运行 async_run，用于接收并分发消息、唤醒挂起的请求。
 * - SECS-I（半双工）当前实现不提供“内部排队/自动串行化”：
 *   1) `async_request/async_send` 请不要并发调用；
 *   2) 若你需要在多线程环境中使用，建议把所有调用统一调度到一个
 *      `asio::strand`（或自行加互斥/发送队列），确保底层串口不会被并发读写。
 *
 * 你提到的典型问题（多线程 Host 并发发送）在这一层的体现是：
 * - SECS-I 后端的底层状态机要求“同一时刻只能有一个收发操作”；并发调用会返回
 *   `invalid_argument`，从而避免把字节流写乱。
 */
class Session final {
public:
    Session(secs::hsms::Session &hsms,
            std::uint16_t session_id,
            SessionOptions options = {});
    Session(secs::secs1::StateMachine &secs1,
            std::uint16_t device_id,
            SessionOptions options = {});

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] Router &router() noexcept { return router_; }
    [[nodiscard]] const Router &router() const noexcept { return router_; }

    void stop() noexcept;

    // 接收循环：持续接收入站消息并处理（匹配挂起请求 / 路由处理器）。
    asio::awaitable<void> async_run();

    /**
     * @brief 单步轮询：接收一条消息并处理（匹配挂起请求 / 路由处理器）。
     *
     * 设计目的：
     * - 适用于需要“自己驱动收发节奏”的场景（尤其是 SECS-I 半双工），例如：
     *   - 主循环里穿插定时发送；
     *   - 需要避免与 async_run 并发导致的串口读写冲突。
     *
     * @return ok 表示成功处理了一条消息；
     *         timeout 表示在给定 timeout 内未收到消息；
     *         其他 error_code 表示接收失败或已 stop。
     */
    asio::awaitable<std::error_code>
    async_poll_once(
        std::optional<secs::core::duration> timeout = std::nullopt);

    // 发送主消息（W=0，不等待回应）。
    asio::awaitable<std::error_code> async_send(std::uint8_t stream,
                                                std::uint8_t function,
                                                secs::core::bytes_view body);

    // 发送主消息（W=1）并等待从消息（T3 超时）。
    asio::awaitable<std::pair<std::error_code, DataMessage>>
    async_request(std::uint8_t stream,
                  std::uint8_t function,
                  secs::core::bytes_view body,
                  std::optional<secs::core::duration> timeout = std::nullopt);

private:
    enum class Backend : std::uint8_t {
        hsms = 0,
        secs1 = 1,
    };

    struct Pending final {
        Pending(std::uint8_t stream, std::uint8_t function)
            : expected_stream(stream), expected_function(function) {}

        std::uint8_t expected_stream{0};
        std::uint8_t expected_function{0};
        secs::core::Event ready{};
        std::error_code ec{};
        std::optional<DataMessage> response{};
    };

    asio::awaitable<std::error_code>
    async_send_message_(const DataMessage &msg);
    asio::awaitable<std::pair<std::error_code, DataMessage>>
    async_receive_message_(std::optional<secs::core::duration> timeout);

    asio::awaitable<void> handle_inbound_(DataMessage msg);
    [[nodiscard]] bool try_fulfill_pending_(DataMessage &msg) noexcept;
    void cancel_all_pending_(std::error_code reason) noexcept;

    void ensure_hsms_run_loop_started_();

    Backend backend_{Backend::hsms};
    asio::any_io_executor executor_{};
    SessionOptions options_{};

    SystemBytes system_bytes_{};
    Router router_{};

    mutable std::mutex pending_mu_{};
    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_{};

    bool stop_requested_{false};
    bool run_loop_active_{false};
    bool run_loop_spawned_{false};
    std::mutex run_mu_{};

    secs::hsms::Session *hsms_{nullptr};
    std::uint16_t hsms_session_id_{0};

    secs::secs1::StateMachine *secs1_{nullptr};
    std::uint16_t secs1_device_id_{0};
};

} // namespace secs::protocol
