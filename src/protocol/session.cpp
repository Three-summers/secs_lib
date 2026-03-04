#include "secs/protocol/session.hpp"

#include "secs/core/error.hpp"
#include "secs/core/metrics.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/secs1/block.hpp"
#include "secs/secs1/state_machine.hpp"

#include "core/spdlog_logger.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <new>
#include <sstream>
#include <string_view>

namespace secs::protocol {
namespace {

/*
 * protocol::Session 实现（协议层统一收发接口）。
 *
 * 本模块把底层传输层（HSMS / SECS-I）统一抽象为 `DataMessage`：
 * - stream/function/w_bit/system_bytes/body
 * - 便于上层用 Router 按 SxFy 注册处理器、发起 request/response。
 *
 * SystemBytes 与请求-响应：
 * - 本端发送 primary 时会分配一个唯一的 system_bytes；
 * - 对端 secondary 必须回显相同 system_bytes，用于匹配挂起请求；
 * - 该策略由 SystemBytes 分配器保证“本端当前在用值唯一”。
 *
 * 并发模型（与 include/secs/protocol/session.hpp 的说明保持一致）：
 * - HSMS（全双工）：只允许一个接收循环读取连接；async_request 会确保 async_run
 *   只启动一次，由 async_run 串行收包并优先 fulfill pending（请求-响应），再把
 *   入站 primary 交给 Router。
 * - SECS-I（半双工）：底层 StateMachine 不提供内部排队；async_request 在等待
 *   secondary 的同时，自己驱动接收并处理可能插入的 primary（按 Router 路由）。
 *
 * 错误处理：
 * - 接收侧任意错误会 cancel 全部 pending，避免调用方协程永久挂起；
 * - 超时以 std::error_code 返回（errc::timeout），不抛异常。
 */

using secs::core::errc;
using secs::core::make_error_code;

enum class DumpDirection : std::uint8_t {
    tx = 0,
    rx = 1,
};

enum class DumpBackend : std::uint8_t {
    hsms = 0,
    secs1 = 1,
};

[[nodiscard]] const char *dump_dir_name_(DumpDirection d) noexcept {
    return (d == DumpDirection::tx) ? "TX" : "RX";
}

[[nodiscard]] const char *dump_backend_name_(DumpBackend b) noexcept {
    return (b == DumpBackend::hsms) ? "HSMS" : "SECS-I";
}

void emit_dump_(const SessionOptions::DumpOptions &opt,
                std::string_view text) noexcept {
    if (!opt.enable) {
        return;
    }
    if (opt.sink) {
        opt.sink(opt.sink_user, text.data(), text.size());
        return;
    }
    // 默认输出：走库内 spdlog（INFO 级别），便于运行时直接看到 dump。
    try {
        SPDLOG_LOGGER_INFO(secs::core::spdlog_logger_raw(), "{}", text);
    } catch (...) {
        // dump 仅用于调试，不应影响业务协程的可用性。
    }
}

[[nodiscard]] std::string dump_banner_(DumpDirection dir, DumpBackend backend) {
    std::ostringstream oss;
    oss << "====================\n";
    oss << "protocol dump: " << dump_dir_name_(dir) << ' '
        << dump_backend_name_(backend) << '\n';
    oss << "====================\n";
    return oss.str();
}

[[nodiscard]] std::string dump_hsms_(DumpDirection dir,
                                    const secs::hsms::Message &msg,
                                    const SessionOptions::DumpOptions &opt) {
    std::vector<secs::core::byte> frame;
    const auto enc = secs::hsms::encode_frame(msg, frame);
    if (enc) {
        std::ostringstream oss;
        oss << dump_banner_(dir, DumpBackend::hsms);
        oss << "HSMS encode_frame failed: " << enc.message() << "\n";
        return oss.str();
    }

    auto out = dump_banner_(dir, DumpBackend::hsms);
    out += secs::utils::dump_hsms_frame(
        secs::core::bytes_view{frame.data(), frame.size()}, opt.hsms);
    return out;
}

[[nodiscard]] std::string dump_secs1_(DumpDirection dir,
                                     const secs::secs1::Header &header,
                                     secs::core::bytes_view body,
                                     const SessionOptions::DumpOptions &opt) {
    auto out = dump_banner_(dir, DumpBackend::secs1);
    out += secs::utils::dump_secs1_message(header, body, opt.secs1);
    return out;
}

[[nodiscard]] bool is_valid_stream(std::uint8_t stream) noexcept {
    return stream <= 0x7FU;
}

[[nodiscard]] bool is_primary_function(std::uint8_t function) noexcept {
    return function != 0U && (function & 0x01U) != 0U;
}

[[nodiscard]] bool
can_compute_secondary_function(std::uint8_t primary_function) noexcept {
    return primary_function != 0xFFU;
}

[[nodiscard]] std::uint8_t
secondary_function(std::uint8_t primary_function) noexcept {
    return static_cast<std::uint8_t>(primary_function + 1U);
}

[[nodiscard]] std::optional<secs::core::duration>
normalize_timeout(secs::core::duration d) noexcept {
    if (d == secs::core::duration{}) {
        return std::nullopt;
    }
    return d;
}

} // namespace

struct Session::State final : std::enable_shared_from_this<State> {
    friend class Session;

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

    State(secs::hsms::Session &hsms,
          std::uint16_t session_id,
          SessionOptions options);
    State(secs::secs1::StateMachine &secs1,
          std::uint16_t device_id,
          SessionOptions options);

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] Router &router() noexcept { return router_; }
    [[nodiscard]] const Router &router() const noexcept { return router_; }

    void stop() noexcept;

    asio::awaitable<void> async_run();
    asio::awaitable<std::error_code>
    async_poll_once(std::optional<secs::core::duration> timeout);
    asio::awaitable<std::error_code>
    async_send(std::uint8_t stream,
               std::uint8_t function,
               secs::core::bytes_view body);
    asio::awaitable<std::pair<std::error_code, DataMessage>>
    async_request(std::uint8_t stream,
                  std::uint8_t function,
                  secs::core::bytes_view body,
                  std::optional<secs::core::duration> timeout);

private:
    asio::awaitable<std::error_code>
    async_send_message_(const DataMessage &msg);
    asio::awaitable<std::pair<std::error_code, DataMessage>>
    async_receive_message_(std::optional<secs::core::duration> timeout);

    asio::awaitable<void> handle_inbound_(DataMessage msg);
    [[nodiscard]] bool try_fulfill_pending_(DataMessage &msg) noexcept;
    void cancel_all_pending_(std::error_code reason) noexcept;

    void ensure_hsms_run_loop_started_();

    // 为了避免 Pending::ready(core::Event) 在多线程 io_context 下出现跨线程并发访问，
    // public API 会把实际逻辑收敛到同一 executor/strand 上执行。
    asio::awaitable<void> async_run_impl_();
    asio::awaitable<std::error_code>
    async_poll_once_impl_(std::optional<secs::core::duration> timeout);
    asio::awaitable<std::error_code>
    async_send_impl_(std::uint8_t stream,
                     std::uint8_t function,
                     secs::core::bytes_view body);
    asio::awaitable<std::pair<std::error_code, DataMessage>>
    async_request_impl_(std::uint8_t stream,
                        std::uint8_t function,
                        secs::core::bytes_view body,
                        std::optional<secs::core::duration> timeout);

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

Session::Session(secs::hsms::Session &hsms,
                 std::uint16_t session_id,
                 SessionOptions options)
    : state_(std::make_shared<State>(hsms, session_id, options)) {}

Session::Session(secs::secs1::StateMachine &secs1,
                 std::uint16_t device_id,
                 SessionOptions options)
    : state_(std::make_shared<State>(secs1, device_id, options)) {}

Session::~Session() noexcept { stop(); }

asio::any_io_executor Session::executor() const noexcept {
    if (!state_) {
        return asio::any_io_executor{};
    }
    return state_->executor();
}

Router &Session::router() noexcept {
    // 约定：Session 构造成功后 state_ 永远非空。
    return state_->router();
}

const Router &Session::router() const noexcept { return state_->router(); }

void Session::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

asio::awaitable<void> Session::async_run() {
    const auto state = state_;
    if (!state) {
        co_return;
    }
    co_await state->async_run();
}

asio::awaitable<std::error_code>
Session::async_poll_once(std::optional<secs::core::duration> timeout) {
    const auto state = state_;
    if (!state) {
        co_return make_error_code(errc::invalid_argument);
    }
    co_return co_await state->async_poll_once(timeout);
}

asio::awaitable<std::error_code>
Session::async_send(std::uint8_t stream,
                    std::uint8_t function,
                    secs::core::bytes_view body) {
    const auto state = state_;
    if (!state) {
        co_return make_error_code(errc::invalid_argument);
    }
    co_return co_await state->async_send(stream, function, body);
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::async_request(std::uint8_t stream,
                       std::uint8_t function,
                       secs::core::bytes_view body,
                       std::optional<secs::core::duration> timeout) {
    const auto state = state_;
    if (!state) {
        co_return std::pair{make_error_code(errc::invalid_argument), DataMessage{}};
    }
    co_return co_await state->async_request(stream, function, body, timeout);
}

Session::State::State(secs::hsms::Session &hsms,
                      std::uint16_t session_id,
                      SessionOptions options)
    : backend_(Backend::hsms),
      executor_(asio::make_strand(hsms.executor())),
      options_(options),
      hsms_(&hsms),
      hsms_session_id_(session_id) {}

Session::State::State(secs::secs1::StateMachine &secs1,
                      std::uint16_t device_id,
                      SessionOptions options)
    : backend_(Backend::secs1),
      executor_(asio::make_strand(secs1.executor())),
      options_(options),
      secs1_(&secs1),
      secs1_device_id_(device_id) {}

void Session::State::ensure_hsms_run_loop_started_() {
    std::lock_guard lk(run_mu_);
    if (run_loop_spawned_) {
        return;
    }
    std::shared_ptr<State> self;
    try {
        self = shared_from_this();
    } catch (...) {
        return;
    }

    run_loop_spawned_ = true;
    try {
        asio::co_spawn(
            executor_,
            [self]() -> asio::awaitable<void> { co_await self->async_run_impl_(); },
            // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
            asio::detached);
    } catch (...) {
        // best-effort：资源不足时允许后续重试启动 run loop。
        run_loop_spawned_ = false;
    }
}

void Session::State::stop() noexcept {
    // 约束：core::Event 默认假设同一 executor/strand 语境，因此 stop() 通过 dispatch
    // 收敛到 Session 自身的 executor_ 执行，避免跨线程直接 cancel waiters。
    std::shared_ptr<State> self;
    try {
        self = shared_from_this();
    } catch (...) {
        return;
    }
    try {
        asio::dispatch(executor_, [self]() noexcept {
            if (self->stop_requested_) {
                return;
            }

            self->stop_requested_ = true;
            self->cancel_all_pending_(make_error_code(errc::cancelled));

            // HSMS 后端：主动取消底层阻塞读，避免依赖 poll_interval 轮询退出。
            if (self->backend_ == Backend::hsms && self->hsms_) {
                self->hsms_->stop();
            }
        });
    } catch (...) {
        // best-effort：dispatch/post 失败通常意味着资源不足，此处不抛异常。
    }
}

asio::awaitable<void> Session::State::async_run() {
    const auto ex = co_await asio::this_coro::executor;
    if (ex != executor_) {
        // 统一切到自身 strand，避免内部状态跨线程并发访问。
        try {
            co_await asio::dispatch(executor_, asio::use_awaitable);
        } catch (...) {
            // best-effort：资源不足等异常不应导致上层协程崩溃。
            co_return;
        }
    }

    co_await async_run_impl_();
}

asio::awaitable<void> Session::State::async_run_impl_() {
    if (run_loop_active_) {
        co_return;
    }

    run_loop_active_ = true;
    struct Reset final {
        State *self;
        ~Reset() { self->run_loop_active_ = false; }
    } reset{this};

    // HSMS：stop() 会主动取消底层读，因此 run loop 可使用“无超时等待”，避免空闲轮询。
    // SECS-I：底层 Link/StateMachine 当前不支持跨线程 cancel，因此仍保留 poll_interval。
    const auto timeout =
        (backend_ == Backend::hsms) ? std::nullopt
                                    : normalize_timeout(options_.poll_interval);

    while (!stop_requested_) {
        auto [ec, msg] = co_await async_receive_message_(timeout);
        if (ec == make_error_code(errc::timeout)) {
            continue;
        }
        if (ec) {
            // HSMS：状态切换在业务上通常是“可恢复事件”（允许上层重连或被动端再次 accept）。
            //
            // 注意 1：对端 SEPARATE 触发的断线在 HSMS 层会被映射为 errc::cancelled。
            // 注意 2：对端 DESELECT 会进入 NOT_SELECTED（state=connected），HSMS 会 cancel
            //         inbound_event_ 来唤醒等待者，同样可能表现为 errc::cancelled。
            //
            // 若此处把 cancelled 视为不可恢复，会导致协议层 run loop 在首次状态切换后永久停止，
            // 从而出现“重连后已 selected 但再也收不到/处理不到业务消息”的现象。
            //
            // 因此这里不直接 stop，而是取消挂起请求后等待下一次 selected（除非本端已 stop）。
            if (backend_ == Backend::hsms && hsms_ && !stop_requested_) {
                cancel_all_pending_(ec);

                while (!stop_requested_) {
                    // 先快路径检查：若已恢复到 selected，则立即继续收包。
                    if (hsms_->state() == secs::hsms::SessionState::selected) {
                        break;
                    }

                    // 以“当前代次+1”为目标等待下一次 selected。
                    // 注意：generation 读取必须放在循环内，避免错过已完成的重连代次。
                    const auto target_gen = hsms_->selected_generation() + 1U;
                    // 使用短周期等待而不是“超长单次等待”：
                    // - 即使极端并发下错过一次 cancel/set，也会在下一轮 timeout 后重新检查 stop/状态；
                    // - 避免 destroy/stop 在少数竞态下被单次长等待拖住。
                    const auto wait_ec =
                        co_await hsms_->async_wait_selected(
                            target_gen, std::chrono::milliseconds{200});
                    if (!wait_ec) {
                        break;
                    }
                    if (wait_ec == make_error_code(errc::timeout)) {
                        continue;
                    }
                    if (wait_ec == make_error_code(errc::cancelled)) {
                        // HSMS stop() 会导致 async_wait_selected 返回 cancelled；此时按不可恢复处理。
                        stop_requested_ = true;
                        cancel_all_pending_(wait_ec);
                        break;
                    }

                    // 其他错误：按不可恢复处理。
                    stop_requested_ = true;
                    cancel_all_pending_(wait_ec);
                    break;
                }
                continue;
            }

            // 其他错误：按不可恢复处理。
            // 先置位 stop，再 cancel pending：避免并发新请求扩大窗口。
            stop_requested_ = true;
            cancel_all_pending_(ec);
            break;
        }
        co_await handle_inbound_(std::move(msg));
    }
}

asio::awaitable<std::error_code>
Session::State::async_poll_once(std::optional<secs::core::duration> timeout) {
    const auto ex = co_await asio::this_coro::executor;
    if (ex != executor_) {
        try {
            co_await asio::dispatch(executor_, asio::use_awaitable);
        } catch (const std::bad_alloc &) {
            co_return make_error_code(errc::out_of_memory);
        } catch (...) {
            co_return make_error_code(errc::invalid_argument);
        }
    }

    co_return co_await async_poll_once_impl_(timeout);
}

asio::awaitable<std::error_code>
Session::State::async_poll_once_impl_(std::optional<secs::core::duration> timeout) {
    if (stop_requested_) {
        co_return make_error_code(errc::cancelled);
    }
    if (run_loop_active_) {
        // 避免与 async_run 并发读同一条底层连接/串口。
        co_return make_error_code(errc::invalid_argument);
    }

    auto [ec, msg] = co_await async_receive_message_(timeout);
    if (ec) {
        co_return ec;
    }

    co_await handle_inbound_(std::move(msg));
    co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::State::async_send(
    std::uint8_t stream, std::uint8_t function, secs::core::bytes_view body) {
    const auto ex = co_await asio::this_coro::executor;
    if (ex != executor_) {
        try {
            co_await asio::dispatch(executor_, asio::use_awaitable);
        } catch (const std::bad_alloc &) {
            co_return make_error_code(errc::out_of_memory);
        } catch (...) {
            co_return make_error_code(errc::invalid_argument);
        }
    }

    co_return co_await async_send_impl_(stream, function, body);
}

asio::awaitable<std::error_code> Session::State::async_send_impl_(
    std::uint8_t stream, std::uint8_t function, secs::core::bytes_view body) {
    static constexpr const char *kCalls = "secs.protocol.send.calls";
    static constexpr const char *kOk = "secs.protocol.send.ok";
    static constexpr const char *kErrors = "secs.protocol.send.errors";
    static constexpr const char *kBodyBytes = "secs.protocol.send.body_bytes";

    secs::core::metrics_counter(kCalls, 1);
    secs::core::metrics_histogram(kBodyBytes,
                                  static_cast<std::uint64_t>(body.size()));

    if (!is_valid_stream(stream) || !is_primary_function(function)) {
        secs::core::metrics_counter(kErrors, 1);
        co_return make_error_code(errc::invalid_argument);
    }

    std::uint32_t sb = 0;
    auto alloc_ec = system_bytes_.allocate(sb);
    if (alloc_ec) {
        secs::core::metrics_counter(kErrors, 1);
        co_return alloc_ec;
    }

    DataMessage msg{};
    msg.stream = stream;
    msg.function = function;
    msg.w_bit = false;
    msg.system_bytes = sb;
    msg.body.assign(body.begin(), body.end());

    SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                        "protocol async_send: S{}F{} W=0 sb={} body_n={}",
                        static_cast<int>(msg.stream),
                        static_cast<int>(msg.function),
                        msg.system_bytes,
                        msg.body.size());

    auto ec = co_await async_send_message_(msg);
    if (ec) {
        secs::core::metrics_counter(kErrors, 1);
        SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                            "protocol async_send failed: sb={} ec={}({})",
                            sb,
                            ec.value(),
                            ec.message());
    } else {
        secs::core::metrics_counter(kOk, 1);
    }
    system_bytes_.release(sb);
    co_return ec;
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::State::async_request(std::uint8_t stream,
                              std::uint8_t function,
                              secs::core::bytes_view body,
                              std::optional<secs::core::duration> timeout) {
    const auto ex = co_await asio::this_coro::executor;
    if (ex != executor_) {
        try {
            co_await asio::dispatch(executor_, asio::use_awaitable);
        } catch (const std::bad_alloc &) {
            co_return std::pair{make_error_code(errc::out_of_memory), DataMessage{}};
        } catch (...) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                DataMessage{}};
        }
    }

    co_return co_await async_request_impl_(stream, function, body, timeout);
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::State::async_request_impl_(std::uint8_t stream,
                                    std::uint8_t function,
                                    secs::core::bytes_view body,
                                    std::optional<secs::core::duration> timeout) {
    static constexpr const char *kCalls = "secs.protocol.request.calls";
    static constexpr const char *kOk = "secs.protocol.request.ok";
    static constexpr const char *kErrors = "secs.protocol.request.errors";
    static constexpr const char *kTimeouts = "secs.protocol.request.timeouts";
    static constexpr const char *kInvalidArg =
        "secs.protocol.request.invalid_argument";
    static constexpr const char *kCancelled = "secs.protocol.request.cancelled";
    static constexpr const char *kPendingOverflow =
        "secs.protocol.request.pending_overflow";
    static constexpr const char *kLatencyMs = "secs.protocol.request.latency_ms";
    static constexpr const char *kBodyBytes = "secs.protocol.request.body_bytes";
    static constexpr const char *kReplyBodyBytes =
        "secs.protocol.reply.body_bytes";
    static constexpr const char *kPendingGauge = "secs.protocol.pending_requests";

    secs::core::metrics_counter(kCalls, 1);
    secs::core::metrics_histogram(kBodyBytes,
                                  static_cast<std::uint64_t>(body.size()));

    if (!is_valid_stream(stream) || !is_primary_function(function) ||
        !can_compute_secondary_function(function)) {
        secs::core::metrics_counter(kErrors, 1);
        secs::core::metrics_counter(kInvalidArg, 1);
        co_return std::pair{make_error_code(errc::invalid_argument),
                            DataMessage{}};
    }
    if (stop_requested_) {
        secs::core::metrics_counter(kErrors, 1);
        secs::core::metrics_counter(kCancelled, 1);
        co_return std::pair{make_error_code(errc::cancelled), DataMessage{}};
    }

    const auto start = secs::core::steady_clock::now();

    const auto expected_function = secondary_function(function);
    const auto t3 = timeout.value_or(options_.t3);

    std::uint32_t sb = 0;
    auto alloc_ec = system_bytes_.allocate(sb);
    if (alloc_ec) {
        secs::core::metrics_counter(kErrors, 1);
        co_return std::pair{alloc_ec, DataMessage{}};
    }

    DataMessage req{};
    req.stream = stream;
    req.function = function;
    req.w_bit = true;
    req.system_bytes = sb;
    req.body.assign(body.begin(), body.end());

    // HSMS：用接收循环统一接收并分发，避免多个请求并发读造成竞争。
    if (backend_ == Backend::hsms) {
        ensure_hsms_run_loop_started_();

        SPDLOG_LOGGER_DEBUG(
            secs::core::spdlog_logger_raw(),
            "protocol async_request(HSMS): S{}F{} -> expect F{} sb={} body_n={}",
            static_cast<int>(stream),
            static_cast<int>(function),
            static_cast<int>(expected_function),
            sb,
            req.body.size());

        auto pending = std::make_shared<Pending>(stream, expected_function);
        {
            std::lock_guard lk(pending_mu_);
            const auto max_pending =
                options_.max_pending_requests == 0 ? std::size_t{1}
                                                   : options_.max_pending_requests;
            if (pending_.size() >= max_pending) {
                secs::core::metrics_counter(kErrors, 1);
                secs::core::metrics_counter(kPendingOverflow, 1);
                system_bytes_.release(sb);
                co_return std::pair{make_error_code(errc::buffer_overflow),
                                    DataMessage{}};
            }
            pending_.insert_or_assign(sb, pending);
            secs::core::metrics_gauge(
                kPendingGauge, static_cast<std::int64_t>(pending_.size()));
        }

        auto send_ec = co_await async_send_message_(req);
        if (send_ec) {
            secs::core::metrics_counter(kErrors, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(HSMS) send failed: sb={} ec={}({})",
                sb,
                send_ec.value(),
                send_ec.message());
            {
                std::lock_guard lk(pending_mu_);
                pending_.erase(sb);
                secs::core::metrics_gauge(
                    kPendingGauge, static_cast<std::int64_t>(pending_.size()));
            }
            system_bytes_.release(sb);
            co_return std::pair{send_ec, DataMessage{}};
        }

        auto wait_ec = co_await pending->ready.async_wait(t3);
        {
            std::lock_guard lk(pending_mu_);
            pending_.erase(sb);
            secs::core::metrics_gauge(
                kPendingGauge, static_cast<std::int64_t>(pending_.size()));
        }
        system_bytes_.release(sb);

        if (wait_ec == make_error_code(errc::timeout)) {
            secs::core::metrics_counter(kErrors, 1);
            secs::core::metrics_counter(kTimeouts, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(HSMS) timeout: sb={} t3_ms={}",
                sb,
                std::chrono::duration_cast<std::chrono::milliseconds>(t3).count());
            co_return std::pair{wait_ec, DataMessage{}};
        }
        if (wait_ec) {
            secs::core::metrics_counter(kErrors, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(HSMS) wait failed: sb={} ec={}({})",
                sb,
                wait_ec.value(),
                wait_ec.message());
            co_return std::pair{pending->ec ? pending->ec : wait_ec,
                                DataMessage{}};
        }
        if (pending->ec) {
            secs::core::metrics_counter(kErrors, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(HSMS) pending failed: sb={} ec={}({})",
                sb,
                pending->ec.value(),
                pending->ec.message());
            co_return std::pair{pending->ec, DataMessage{}};
        }
        if (!pending->response.has_value()) {
            secs::core::metrics_counter(kErrors, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(HSMS) pending has no response: sb={}",
                sb);
            co_return std::pair{make_error_code(errc::invalid_argument),
                                DataMessage{}};
        }
        SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                            "protocol async_request(HSMS) done: sb={}",
                            sb);
        secs::core::metrics_counter(kOk, 1);
        secs::core::metrics_histogram(
            kLatencyMs,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    secs::core::steady_clock::now() - start)
                    .count()));
        secs::core::metrics_histogram(
            kReplyBodyBytes,
            static_cast<std::uint64_t>(pending->response->body.size()));
        co_return std::pair{std::error_code{}, *pending->response};
    }

    // SECS-I：半双工，请求侧自己驱动接收循环，并在期间处理可能的入站主消息。
    SPDLOG_LOGGER_DEBUG(
        secs::core::spdlog_logger_raw(),
        "protocol async_request(SECS-I): S{}F{} -> expect F{} sb={} body_n={}",
        static_cast<int>(stream),
        static_cast<int>(function),
        static_cast<int>(expected_function),
        sb,
        req.body.size());
    auto send_ec = co_await async_send_message_(req);
    if (send_ec) {
        secs::core::metrics_counter(kErrors, 1);
        SPDLOG_LOGGER_DEBUG(
            secs::core::spdlog_logger_raw(),
            "protocol async_request(SECS-I) send failed: sb={} ec={}({})",
            sb,
            send_ec.value(),
            send_ec.message());
        system_bytes_.release(sb);
        co_return std::pair{send_ec, DataMessage{}};
    }

    const auto deadline = secs::core::steady_clock::now() + t3;
    for (;;) {
        const auto now = secs::core::steady_clock::now();
        if (now >= deadline) {
            secs::core::metrics_counter(kErrors, 1);
            secs::core::metrics_counter(kTimeouts, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(SECS-I) timeout: sb={} t3_ms={}",
                sb,
                std::chrono::duration_cast<std::chrono::milliseconds>(t3).count());
            system_bytes_.release(sb);
            co_return std::pair{make_error_code(errc::timeout), DataMessage{}};
        }

        const auto remaining = deadline - now;
        auto [ec, msg] = co_await async_receive_message_(remaining);
        if (ec) {
            secs::core::metrics_counter(kErrors, 1);
            SPDLOG_LOGGER_DEBUG(
                secs::core::spdlog_logger_raw(),
                "protocol async_request(SECS-I) receive failed: sb={} ec={}({})",
                sb,
                ec.value(),
                ec.message());
            system_bytes_.release(sb);
            co_return std::pair{ec, DataMessage{}};
        }

        const bool matches = msg.is_secondary() && !msg.w_bit &&
                             msg.system_bytes == sb && msg.stream == stream &&
                             msg.function == expected_function;

        if (matches) {
            SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                                "protocol async_request(SECS-I) done: sb={}",
                                sb);
            system_bytes_.release(sb);
            secs::core::metrics_counter(kOk, 1);
            secs::core::metrics_histogram(
                kLatencyMs,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        secs::core::steady_clock::now() - start)
                        .count()));
            secs::core::metrics_histogram(
                kReplyBodyBytes,
                static_cast<std::uint64_t>(msg.body.size()));
            co_return std::pair{std::error_code{}, std::move(msg)};
        }

        co_await handle_inbound_(std::move(msg));
    }
}

asio::awaitable<std::error_code>
Session::State::async_send_message_(const DataMessage &msg) {
    if (stop_requested_) {
        co_return make_error_code(errc::cancelled);
    }

    if (backend_ == Backend::hsms) {
        if (!hsms_) {
            co_return make_error_code(errc::invalid_argument);
        }
        const auto wire = secs::hsms::make_data_message(
            hsms_session_id_,
            msg.stream,
            msg.function,
            msg.w_bit,
            msg.system_bytes,
            secs::core::bytes_view{msg.body.data(), msg.body.size()});
        if (options_.dump.enable && options_.dump.dump_tx) {
            emit_dump_(options_.dump, dump_hsms_(DumpDirection::tx, wire, options_.dump));
        }
        auto ec = co_await hsms_->async_send(wire);
        if (!ec && options_.tap.enable && options_.tap.tap_tx &&
            options_.tap.on_message) {
            options_.tap.on_message(options_.tap.on_message_user, msg, true);
        }
        co_return ec;
    }

    if (!secs1_) {
        co_return make_error_code(errc::invalid_argument);
    }

    secs::secs1::Header h{};
    h.reverse_bit = options_.secs1_reverse_bit;
    h.device_id = secs1_device_id_;
    h.wait_bit = msg.w_bit;
    h.stream = msg.stream;
    h.function = msg.function;
    h.end_bit = true;
    h.block_number = 1;
    h.system_bytes = msg.system_bytes;

    if (options_.dump.enable && options_.dump.dump_tx) {
        emit_dump_(options_.dump,
                   dump_secs1_(DumpDirection::tx,
                              h,
                              secs::core::bytes_view{msg.body.data(), msg.body.size()},
                              options_.dump));
    }
    auto ec = co_await secs1_->async_send(
        h, secs::core::bytes_view{msg.body.data(), msg.body.size()});
    if (!ec && options_.tap.enable && options_.tap.tap_tx &&
        options_.tap.on_message) {
        options_.tap.on_message(options_.tap.on_message_user, msg, true);
    }
    co_return ec;
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::State::async_receive_message_(std::optional<secs::core::duration> timeout) {
    if (stop_requested_) {
        co_return std::pair{make_error_code(errc::cancelled), DataMessage{}};
    }

    if (backend_ == Backend::hsms) {
        if (!hsms_) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                DataMessage{}};
        }

        auto [ec, msg] = co_await hsms_->async_receive_data(timeout);
        if (ec) {
            co_return std::pair{ec, DataMessage{}};
        }

        if (options_.dump.enable && options_.dump.dump_rx) {
            emit_dump_(options_.dump,
                       dump_hsms_(DumpDirection::rx, msg, options_.dump));
        }

        DataMessage out{};
        out.stream = msg.stream();
        out.function = msg.function();
        out.w_bit = msg.w_bit();
        out.system_bytes = msg.header.system_bytes;
        out.body = std::move(msg.body);
        if (options_.tap.enable && options_.tap.tap_rx && options_.tap.on_message) {
            options_.tap.on_message(options_.tap.on_message_user, out, false);
        }
        co_return std::pair{std::error_code{}, std::move(out)};
    }

    if (!secs1_) {
        co_return std::pair{make_error_code(errc::invalid_argument),
                            DataMessage{}};
    }

    auto [ec, msg] = co_await secs1_->async_receive(timeout);
    if (ec) {
        co_return std::pair{ec, DataMessage{}};
    }

    if (options_.dump.enable && options_.dump.dump_rx) {
        emit_dump_(options_.dump,
                   dump_secs1_(DumpDirection::rx,
                              msg.header,
                              secs::core::bytes_view{msg.body.data(), msg.body.size()},
                              options_.dump));
    }

    DataMessage out{};
    out.stream = msg.header.stream;
    out.function = msg.header.function;
    out.w_bit = msg.header.wait_bit;
    out.system_bytes = msg.header.system_bytes;
    out.body = std::move(msg.body);
    if (options_.tap.enable && options_.tap.tap_rx && options_.tap.on_message) {
        options_.tap.on_message(options_.tap.on_message_user, out, false);
    }
    co_return std::pair{std::error_code{}, std::move(out)};
}

asio::awaitable<void> Session::State::handle_inbound_(DataMessage msg) {
    if (try_fulfill_pending_(msg)) {
        co_return;
    }

    // 未匹配的从消息：忽略（可能是迟到回应/对端异常发送）。
    if (msg.is_secondary()) {
        co_return;
    }

    auto handler_opt = router_.find(msg.stream, msg.function);
    if (!handler_opt.has_value()) {
        SPDLOG_LOGGER_DEBUG(
            secs::core::spdlog_logger_raw(),
            "protocol inbound primary unhandled: S{}F{} W={} sb={} body_n={}",
            static_cast<int>(msg.stream),
            static_cast<int>(msg.function),
            msg.w_bit ? 1 : 0,
            msg.system_bytes,
            msg.body.size());
        co_return;
    }

    auto handler = std::move(*handler_opt);
    SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                        "protocol inbound primary dispatch: S{}F{} W={} sb={} body_n={}",
                        static_cast<int>(msg.stream),
                        static_cast<int>(msg.function),
                        msg.w_bit ? 1 : 0,
                        msg.system_bytes,
                        msg.body.size());
    auto [ec, rsp_body] = co_await handler(msg);
    if (ec) {
        SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                            "protocol handler returned error: S{}F{} sb={} ec={}({})",
                            static_cast<int>(msg.stream),
                            static_cast<int>(msg.function),
                            msg.system_bytes,
                            ec.value(),
                            ec.message());
        co_return;
    }

    if (!msg.w_bit) {
        co_return;
    }
    if (!can_compute_secondary_function(msg.function)) {
        co_return;
    }

    DataMessage rsp{};
    rsp.stream = msg.stream;
    rsp.function = secondary_function(msg.function);
    rsp.w_bit = false;
    rsp.system_bytes = msg.system_bytes;
    rsp.body = std::move(rsp_body);
    SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                        "protocol auto-reply secondary: S{}F{} sb={} body_n={}",
                        static_cast<int>(rsp.stream),
                        static_cast<int>(rsp.function),
                        rsp.system_bytes,
                        rsp.body.size());
    (void)co_await async_send_message_(rsp);
}

bool Session::State::try_fulfill_pending_(DataMessage &msg) noexcept {
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lk(pending_mu_);
        const auto it = pending_.find(msg.system_bytes);
        if (it == pending_.end()) {
            return false;
        }
        pending = it->second;
    }

    // pending_ 的写入只来自
    // async_request（make_shared），因此这里不做空指针分支。
    if (!msg.is_secondary() || msg.w_bit) {
        return false;
    }
    if (pending->expected_stream != msg.stream ||
        pending->expected_function != msg.function) {
        return false;
    }

    SPDLOG_LOGGER_DEBUG(secs::core::spdlog_logger_raw(),
                        "protocol fulfill pending: S{}F{} sb={}",
                        static_cast<int>(msg.stream),
                        static_cast<int>(msg.function),
                        msg.system_bytes);

    pending->response = std::move(msg);
    pending->ec = std::error_code{};
    pending->ready.set();
    return true;
}

void Session::State::cancel_all_pending_(std::error_code reason) noexcept {
    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> moved;
    {
        std::lock_guard lk(pending_mu_);
        moved.swap(pending_);
    }

    for (auto &[sb, pending] : moved) {
        (void)sb;
        pending->ec = reason;
        pending->ready.cancel();
    }
}

} // namespace secs::protocol
