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

    // 接收循环的轮询间隔（仅 async_run/SECS-I 后端使用）：
    // - SECS-I 底层 Link/StateMachine 当前不支持主动 cancel，因此 async_run 需要
    //   通过轮询超时来检查 stop() 并避免永久阻塞。
    // - HSMS 后端由 stop() 主动取消底层读，不依赖轮询。
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

    /**
     * @brief 结构化消息观测（录制/统计用途）。
     *
     * 说明：
     * - 与 dump 不同：dump 输出的是“格式化字符串”；tap 输出的是结构化 DataMessage；
     * - 回调在“协议层收发成功后”触发（发送成功 / 接收成功）；
     * - 回调建议保持轻量、无阻塞；如需 IO（写文件等），建议内部自行做缓冲或异步化；
     * - 回调参数 msg 为引用，仅在回调调用期间有效（请勿保存引用/指针）。
     */
    struct TapOptions final {
        bool enable{false};
        bool tap_tx{true};
        bool tap_rx{true};

        using TapFn =
            void (*)(void *user, const DataMessage &msg, bool is_tx) noexcept;
        TapFn on_message{nullptr};
        void *on_message_user{nullptr};
    };

    TapOptions tap{};
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

    // 析构时会 best-effort 请求 stop()（不等待 async_run 退出）。
    ~Session() noexcept;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) = delete;
    Session &operator=(Session &&) = delete;

    [[nodiscard]] asio::any_io_executor executor() const noexcept;

    [[nodiscard]] Router &router() noexcept;
    [[nodiscard]] const Router &router() const noexcept;

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
    struct State;
    std::shared_ptr<State> state_{};
};

} // namespace secs::protocol
