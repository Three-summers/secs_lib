#include "secs/protocol/session.hpp"

#include "secs/core/error.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/secs1/block.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <chrono>
#include <sstream>
#include <string_view>
#include <spdlog/spdlog.h>

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
        SPDLOG_INFO("{}", text);
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

Session::Session(secs::hsms::Session &hsms,
                 std::uint16_t session_id,
                 SessionOptions options)
    : backend_(Backend::hsms), executor_(hsms.executor()), options_(options),
      hsms_(&hsms), hsms_session_id_(session_id) {}

Session::Session(secs::secs1::StateMachine &secs1,
                 std::uint16_t device_id,
                 SessionOptions options)
    : backend_(Backend::secs1), executor_(secs1.executor()), options_(options),
      secs1_(&secs1), secs1_device_id_(device_id) {}

void Session::ensure_hsms_run_loop_started_() {
    std::lock_guard lk(run_mu_);
    if (run_loop_spawned_) {
        return;
    }
    run_loop_spawned_ = true;

    asio::co_spawn(
        executor_,
        [this]() -> asio::awaitable<void> {
            co_await async_run();
        }, // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
        asio::detached);
}

void Session::stop() noexcept {
    stop_requested_ = true;
    cancel_all_pending_(make_error_code(errc::cancelled));
}

asio::awaitable<void> Session::async_run() {
    if (run_loop_active_) {
        co_return;
    }

    run_loop_active_ = true;
    struct Reset final {
        Session *self;
        ~Reset() { self->run_loop_active_ = false; }
    } reset{this};

    const auto timeout = normalize_timeout(options_.poll_interval);

    while (!stop_requested_) {
        auto [ec, msg] = co_await async_receive_message_(timeout);
        if (ec == make_error_code(errc::timeout)) {
            continue;
        }
        if (ec) {
            // 先置位 stop，再 cancel pending：避免并发新请求扩大窗口。
            stop_requested_ = true;
            cancel_all_pending_(ec);
            break;
        }
        co_await handle_inbound_(std::move(msg));
    }
}

asio::awaitable<std::error_code> Session::async_send(
    std::uint8_t stream, std::uint8_t function, secs::core::bytes_view body) {
    if (!is_valid_stream(stream) || !is_primary_function(function)) {
        co_return make_error_code(errc::invalid_argument);
    }

    std::uint32_t sb = 0;
    auto alloc_ec = system_bytes_.allocate(sb);
    if (alloc_ec) {
        co_return alloc_ec;
    }

    DataMessage msg{};
    msg.stream = stream;
    msg.function = function;
    msg.w_bit = false;
    msg.system_bytes = sb;
    msg.body.assign(body.begin(), body.end());

    SPDLOG_DEBUG("protocol async_send: S{}F{} W=0 sb={} body_n={}",
                 static_cast<int>(msg.stream),
                 static_cast<int>(msg.function),
                 msg.system_bytes,
                 msg.body.size());

    auto ec = co_await async_send_message_(msg);
    if (ec) {
        SPDLOG_DEBUG("protocol async_send failed: sb={} ec={}({})",
                     sb,
                     ec.value(),
                     ec.message());
    }
    system_bytes_.release(sb);
    co_return ec;
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::async_request(std::uint8_t stream,
                       std::uint8_t function,
                       secs::core::bytes_view body,
                       std::optional<secs::core::duration> timeout) {
    if (!is_valid_stream(stream) || !is_primary_function(function) ||
        !can_compute_secondary_function(function)) {
        co_return std::pair{make_error_code(errc::invalid_argument),
                            DataMessage{}};
    }
    if (stop_requested_) {
        co_return std::pair{make_error_code(errc::cancelled), DataMessage{}};
    }

    const auto expected_function = secondary_function(function);
    const auto t3 = timeout.value_or(options_.t3);

    std::uint32_t sb = 0;
    auto alloc_ec = system_bytes_.allocate(sb);
    if (alloc_ec) {
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

        SPDLOG_DEBUG("protocol async_request(HSMS): S{}F{} -> expect F{} sb={} body_n={}",
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
                system_bytes_.release(sb);
                co_return std::pair{make_error_code(errc::buffer_overflow),
                                    DataMessage{}};
            }
            pending_.insert_or_assign(sb, pending);
        }

        auto send_ec = co_await async_send_message_(req);
        if (send_ec) {
            SPDLOG_DEBUG("protocol async_request(HSMS) send failed: sb={} ec={}({})",
                         sb,
                         send_ec.value(),
                         send_ec.message());
            {
                std::lock_guard lk(pending_mu_);
                pending_.erase(sb);
            }
            system_bytes_.release(sb);
            co_return std::pair{send_ec, DataMessage{}};
        }

        auto wait_ec = co_await pending->ready.async_wait(t3);
        {
            std::lock_guard lk(pending_mu_);
            pending_.erase(sb);
        }
        system_bytes_.release(sb);

        if (wait_ec == make_error_code(errc::timeout)) {
            SPDLOG_DEBUG("protocol async_request(HSMS) timeout: sb={} t3_ms={}",
                         sb,
                         std::chrono::duration_cast<std::chrono::milliseconds>(t3)
                             .count());
            co_return std::pair{wait_ec, DataMessage{}};
        }
        if (wait_ec) {
            SPDLOG_DEBUG("protocol async_request(HSMS) wait failed: sb={} ec={}({})",
                         sb,
                         wait_ec.value(),
                         wait_ec.message());
            co_return std::pair{pending->ec ? pending->ec : wait_ec,
                                DataMessage{}};
        }
        if (pending->ec) {
            SPDLOG_DEBUG("protocol async_request(HSMS) pending failed: sb={} ec={}({})",
                         sb,
                         pending->ec.value(),
                         pending->ec.message());
            co_return std::pair{pending->ec, DataMessage{}};
        }
        if (!pending->response.has_value()) {
            SPDLOG_DEBUG("protocol async_request(HSMS) pending has no response: sb={}",
                         sb);
            co_return std::pair{make_error_code(errc::invalid_argument),
                                DataMessage{}};
        }
        SPDLOG_DEBUG("protocol async_request(HSMS) done: sb={}", sb);
        co_return std::pair{std::error_code{}, *pending->response};
    }

    // SECS-I：半双工，请求侧自己驱动接收循环，并在期间处理可能的入站主消息。
    SPDLOG_DEBUG("protocol async_request(SECS-I): S{}F{} -> expect F{} sb={} body_n={}",
                 static_cast<int>(stream),
                 static_cast<int>(function),
                 static_cast<int>(expected_function),
                 sb,
                 req.body.size());
    auto send_ec = co_await async_send_message_(req);
    if (send_ec) {
        SPDLOG_DEBUG("protocol async_request(SECS-I) send failed: sb={} ec={}({})",
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
            SPDLOG_DEBUG("protocol async_request(SECS-I) timeout: sb={} t3_ms={}",
                         sb,
                         std::chrono::duration_cast<std::chrono::milliseconds>(t3)
                             .count());
            system_bytes_.release(sb);
            co_return std::pair{make_error_code(errc::timeout), DataMessage{}};
        }

        const auto remaining = deadline - now;
        auto [ec, msg] = co_await async_receive_message_(remaining);
        if (ec) {
            SPDLOG_DEBUG("protocol async_request(SECS-I) receive failed: sb={} ec={}({})",
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
            SPDLOG_DEBUG("protocol async_request(SECS-I) done: sb={}", sb);
            system_bytes_.release(sb);
            co_return std::pair{std::error_code{}, std::move(msg)};
        }

        co_await handle_inbound_(std::move(msg));
    }
}

asio::awaitable<std::error_code>
Session::async_send_message_(const DataMessage &msg) {
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
        co_return co_await hsms_->async_send(wire);
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
    co_return co_await secs1_->async_send(
        h, secs::core::bytes_view{msg.body.data(), msg.body.size()});
}

asio::awaitable<std::pair<std::error_code, DataMessage>>
Session::async_receive_message_(std::optional<secs::core::duration> timeout) {
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
    co_return std::pair{std::error_code{}, std::move(out)};
}

asio::awaitable<void> Session::handle_inbound_(DataMessage msg) {
    if (try_fulfill_pending_(msg)) {
        co_return;
    }

    // 未匹配的从消息：忽略（可能是迟到回应/对端异常发送）。
    if (msg.is_secondary()) {
        co_return;
    }

    auto handler_opt = router_.find(msg.stream, msg.function);
    if (!handler_opt.has_value()) {
        SPDLOG_DEBUG("protocol inbound primary unhandled: S{}F{} W={} sb={} body_n={}",
                     static_cast<int>(msg.stream),
                     static_cast<int>(msg.function),
                     msg.w_bit ? 1 : 0,
                     msg.system_bytes,
                     msg.body.size());
        co_return;
    }

    auto handler = std::move(*handler_opt);
    SPDLOG_DEBUG("protocol inbound primary dispatch: S{}F{} W={} sb={} body_n={}",
                 static_cast<int>(msg.stream),
                 static_cast<int>(msg.function),
                 msg.w_bit ? 1 : 0,
                 msg.system_bytes,
                 msg.body.size());
    auto [ec, rsp_body] = co_await handler(msg);
    if (ec) {
        SPDLOG_DEBUG("protocol handler returned error: S{}F{} sb={} ec={}({})",
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
    SPDLOG_DEBUG("protocol auto-reply secondary: S{}F{} sb={} body_n={}",
                 static_cast<int>(rsp.stream),
                 static_cast<int>(rsp.function),
                 rsp.system_bytes,
                 rsp.body.size());
    (void)co_await async_send_message_(rsp);
}

bool Session::try_fulfill_pending_(DataMessage &msg) noexcept {
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

    SPDLOG_DEBUG("protocol fulfill pending: S{}F{} sb={}",
                 static_cast<int>(msg.stream),
                 static_cast<int>(msg.function),
                 msg.system_bytes);

    pending->response = std::move(msg);
    pending->ec = std::error_code{};
    pending->ready.set();
    return true;
}

void Session::cancel_all_pending_(std::error_code reason) noexcept {
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
