#include "secs/hsms/session.hpp"

#include "secs/core/error.hpp"
#include "secs/hsms/timer.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <spdlog/spdlog.h>
#include <vector>

namespace secs::hsms {
namespace {

constexpr std::uint16_t kHsmsSsControlSessionId = 0xFFFF;

constexpr std::uint8_t kRspOk = 0;
constexpr std::uint8_t kRspReject = 1;

} // namespace

/*
 * HSMS::Session 的协程并发模型（便于理解“为什么要有
 * pending_/Event/reader_loop_”）：
 *
 * 1) 读方向：start_reader_() 启动一个常驻 reader_loop_ 协程，串行读取
 * connection_ 的 HSMS 帧。
 *
 * 2) 控制事务/数据事务：
 *    - async_control_transaction_ / async_data_transaction_ 在发送请求前，把
 * (system_bytes -> Pending) 登记到 pending_。
 *    - reader_loop_ 收到响应后，通过 fulfill_pending_() 唤醒对应
 * Pending::ready。
 *
 * 3) 数据消息队列：
 *    - 未被 pending_ 消费的 data message 会进入 inbound_data_；
 *    - async_receive_data() 只负责从 inbound_data_ 取“下一条 data”交给上层。
 *
 * 4) 断线处理：
 *    - on_disconnected_ 统一取消 selected_event_/inbound_event_ 以及所有
 * pending， 避免协程永久挂起。
 *
 * 5) selected_generation_：
 *    - 每次进入 selected 都会递增 generation，用于让旧连接周期的 linktest_loop_
 * 在重连后自动退出。
 */
// 初始状态下未启动 reader_loop_，因此 reader_stopped_event_
// 置为已完成（避免等待方无意义阻塞）。 该事件会在 start_reader_() 时
// reset，并在 reader_loop_ 退出时 set。
Session::Session(asio::any_io_executor ex, SessionOptions options)
    : executor_(ex), options_(options),
      connection_(ex, ConnectionOptions{.t8 = options.t8}) {
    reader_stopped_event_.set();
}

void Session::reset_state_() noexcept {
    state_ = SessionState::connected;
    connection_.disable_data_writes(core::make_error_code(core::errc::cancelled));

    selected_event_.reset();
    inbound_event_.reset();
    disconnected_event_.reset();

    inbound_data_.clear();
    pending_.clear();
}

void Session::set_selected_() noexcept {
    if (state_ == SessionState::selected) {
        return;
    }
    state_ = SessionState::selected;
    connection_.enable_data_writes();
    const auto gen = selected_generation_.fetch_add(1U) + 1U;
    selected_event_.set();
    SPDLOG_DEBUG("hsms selected: generation={}", gen);

    if (options_.linktest_interval != core::duration{}) {
        try {
            asio::co_spawn(
                executor_,
                [this, gen]() -> asio::awaitable<void> {
                    co_await linktest_loop_(gen);
                }, // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
                asio::detached);
        } catch (...) {
            // co_spawn 可能因资源不足抛异常；在 noexcept 路径中必须吞掉，
            // 否则会触发 std::terminate。失败时仅意味着自动 LINKTEST 不可用。
        }
    }
}

void Session::set_not_selected_() noexcept {
    if (state_ == SessionState::disconnected) {
        return;
    }

    state_ = SessionState::connected;
    connection_.disable_data_writes(core::make_error_code(core::errc::cancelled));

    // 退出 selected 后必须 reset selected_event_，否则等待方可能进入忙等。
    selected_event_.reset();

    // NOT_SELECTED 期间不应继续向上层交付 data message；同时唤醒等待者。
    inbound_data_.clear();
    inbound_event_.cancel();
    inbound_event_.reset();

    cancel_pending_data_(core::make_error_code(core::errc::cancelled));
    SPDLOG_DEBUG("hsms not-selected");
}

void Session::on_disconnected_(std::error_code reason) noexcept {
    SPDLOG_DEBUG("hsms disconnected: ec={}({})", reason.value(), reason.message());
    state_ = SessionState::disconnected;
    connection_.disable_data_writes(reason);

    // 唤醒所有等待者：已选择（selected）状态等待、入站队列等待、挂起事务等待。
    selected_event_.cancel();
    selected_event_.reset();
    inbound_event_.cancel();
    inbound_event_.reset();
    reader_running_ = false;
    disconnected_event_.set();

    for (auto &[_, pending] : pending_) {
        pending->ec = reason;
        pending->ready.cancel();
    }
    pending_.clear();
    inbound_data_.clear();
}

void Session::stop() noexcept {
    stop_requested_ = true;
    connection_.cancel_and_close();

    SPDLOG_DEBUG("hsms stop requested");
    on_disconnected_(core::make_error_code(core::errc::cancelled));
}

void Session::start_reader_() {
    reader_running_ = true;
    disconnected_event_.reset();
    reader_stopped_event_.reset();

    asio::co_spawn(
        executor_,
        [this]() -> asio::awaitable<void> {
            co_await reader_loop_();
        }, // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
        asio::detached);
}

asio::awaitable<void> Session::reader_loop_() {
    while (!stop_requested_) {
        auto [ec, msg] = co_await connection_.async_read_message();
        if (ec) {
            connection_.cancel_and_close();
            // reader_loop_ 退出路径可能与外部触发的 on_disconnected_ 重叠（例如
            // stop()、T6 超时主动断线）。为了避免“第二次断线原因覆盖第一次”的
            // 语义漂移，这里只在尚未进入 disconnected 时调用 on_disconnected_。
            if (state_ != SessionState::disconnected) {
                on_disconnected_(ec);
            }
            break;
        }

        if (msg.is_data()) {
            if (fulfill_pending_(msg)) {
                continue;
            }
            if (state_ != SessionState::selected) {
                // NOT_SELECTED 期间收到 data：按协议不向上层交付（直接丢弃）。
                continue;
            }
            inbound_data_.push_back(std::move(msg));
            inbound_event_.set();
            continue;
        }

        // 控制消息：SELECT/DESELECT/LINKTEST/SEPARATE 等。
        if (msg.header.session_id != kHsmsSsControlSessionId) {
            // HSMS-SS 约束：控制消息的 SessionID 固定为 0xFFFF。
            // 若对端发来其它值，通常意味着：
            // - 对端实现不是 HSMS-SS（可能是 HSMS-GS），或
            // - 对端实现有 bug/字段填充错误。
            // 当前库选择“断线收敛”，避免会话进入不可预测状态。
            (void)co_await connection_.async_close();
            on_disconnected_(
                core::make_error_code(core::errc::invalid_argument));
            break;
        }

        bool should_exit = false;
        switch (msg.header.s_type) {
        case SType::select_req: {
            // 被动端收到 SELECT.req：
            // - 可按配置拒绝（用于单元测试覆盖）
            // - 校验 SessionID（HSMS-SS：固定 0xFFFF，不匹配则拒绝并断线）
            // - 已处于 selected 时重复收到 SELECT.req：回拒绝，但保持连接
            if (!options_.passive_accept_select) {
                (void)co_await connection_.async_write_message(
                    make_select_rsp(msg.header.session_id,
                                    kRspReject,
                                    msg.header.system_bytes));
                // 仅拒绝 SELECT，不进入 selected；保持连接处于 NOT_SELECTED，
                // 便于对端读取到拒绝响应并自行决定后续行为（例如断线/重试）。
                set_not_selected_();
                break;
            }

            if (state_ == SessionState::selected) {
                (void)co_await connection_.async_write_message(
                    make_select_rsp(msg.header.session_id,
                                    kRspReject,
                                    msg.header.system_bytes));
                break;
            }

            if (msg.header.session_id != kHsmsSsControlSessionId) {
                (void)co_await connection_.async_write_message(
                    make_select_rsp(msg.header.session_id,
                                    kRspReject,
                                    msg.header.system_bytes));
                (void)co_await connection_.async_close();
                on_disconnected_(
                    core::make_error_code(core::errc::invalid_argument));
                should_exit = true;
                break;
            }

            (void)co_await connection_.async_write_message(
                make_select_rsp(msg.header.session_id,
                                kRspOk,
                                msg.header.system_bytes));
            set_selected_();
            break;
        }
        case SType::select_rsp: {
            // 主动端收到 SELECT.rsp：
            // - 先尝试唤醒挂起的控制事务（对应 async_control_transaction_）
            // - 若接受则进入 selected
            const bool matched = fulfill_pending_(msg);
            if (matched && msg.header.header_byte2 == kRspOk) {
                set_selected_();
            }
            break;
        }
        case SType::deselect_req: {
            // 收到 DESELECT.req：先立即阻断 data，再回复 DESELECT.rsp，并退回 NOT_SELECTED。
            set_not_selected_();
            (void)co_await connection_.async_write_message(
                make_deselect_rsp(msg.header.session_id,
                                  kRspOk,
                                  msg.header.system_bytes));
            break;
        }
        case SType::deselect_rsp: {
            (void)fulfill_pending_(msg);
            // 对端确认 DESELECT：退回 NOT_SELECTED（保持连接 open）。
            set_not_selected_();
            break;
        }
        case SType::linktest_req: {
            // 收到 LINKTEST.req：立即回复 LINKTEST.rsp（不进入 inbound_data_）
            (void)co_await connection_.async_write_message(
                make_linktest_rsp(msg.header.session_id, msg.header.system_bytes));
            break;
        }
        case SType::linktest_rsp: {
            (void)fulfill_pending_(msg);
            break;
        }
        case SType::reject_req: {
            // Reject.req：当前不向上层暴露，仅保持会话稳定（后续可扩展 reason code
            // 解析/回调）。
            break;
        }
        case SType::separate_req: {
            (void)co_await connection_.async_close();
            on_disconnected_(core::make_error_code(core::errc::cancelled));
            should_exit = true;
            break;
        }
        default: {
            // 未实现/未知的控制类型：按 Reject 反馈，避免对端只能靠断线诊断。
            (void)co_await connection_.async_write_message(
                make_reject_req(/*reason_code=*/1, msg.header));
            break;
        }
        }

        if (should_exit) {
            break;
        }
    }

    reader_running_ = false;
    reader_stopped_event_.set();
}

asio::awaitable<void> Session::linktest_loop_(std::uint64_t generation) {
    std::uint32_t consecutive_failures = 0;
    const std::uint32_t max_failures =
        std::max<std::uint32_t>(1U, options_.linktest_max_consecutive_failures);

    while (!stop_requested_) {
        if (state_ != SessionState::selected ||
            selected_generation_.load() != generation) {
            co_return;
        }

        // 使用 disconnected_event_ 来实现“可取消的周期等待”：
        // - 正常情况下：等待超时（timeout）-> 发送一次 LINKTEST
        // - stop()/断线：disconnected_event_ 被 set -> 立即退出，避免长期 timer
        // 挂起导致无法安全销毁
        auto ec =
            co_await disconnected_event_.async_wait(options_.linktest_interval);
        if (!ec) { // 已断线
            co_return;
        }
        if (ec != core::make_error_code(core::errc::timeout)) {
            co_return;
        }

        if (state_ != SessionState::selected ||
            selected_generation_.load() != generation) {
            co_return;
        }

        ec = co_await async_linktest();
        if (ec) {
            ++consecutive_failures;
            if (consecutive_failures >= max_failures) {
                (void)co_await connection_.async_close();
                co_return;
            }
            continue;
        }
        consecutive_failures = 0;
    }
}

asio::awaitable<std::error_code>
Session::async_wait_reader_stopped(std::optional<core::duration> timeout) {
    co_return co_await reader_stopped_event_.async_wait(timeout);
}

bool Session::fulfill_pending_(Message &msg) noexcept {
    const auto it = pending_.find(msg.header.system_bytes);
    if (it == pending_.end()) {
        return false;
    }

    const auto &pending = it->second;

    if (pending->expected_stype != msg.header.s_type) {
        return false;
    }

    pending->response = std::move(msg);
    pending->ec = std::error_code{};
    pending->ready.set();
    return true;
}

void Session::cancel_pending_data_(std::error_code reason) noexcept {
    std::vector<std::uint32_t> to_erase;
    to_erase.reserve(pending_.size());

    for (const auto &[sb, pending] : pending_) {
        if (pending->expected_stype != SType::data) {
            continue;
        }
        pending->ec = reason;
        pending->ready.cancel();
        to_erase.push_back(sb);
    }

    for (const auto sb : to_erase) {
        pending_.erase(sb);
    }
}

asio::awaitable<std::pair<std::error_code, Message>>
Session::async_control_transaction_(const Message &req,
                                    SType expected_rsp,
                                    core::duration timeout) {
    // 控制事务：把请求登记到 pending_，由 reader_loop_ 收到响应后唤醒。
    const auto max_pending =
        options_.max_pending_requests == 0 ? std::size_t{1}
                                           : options_.max_pending_requests;
    if (pending_.size() >= max_pending) {
        co_return std::pair{core::make_error_code(core::errc::buffer_overflow),
                            Message{}};
    }

    const auto sb = req.header.system_bytes;
    auto pending = std::make_shared<Pending>(expected_rsp);
    pending_.insert_or_assign(sb, pending);

    auto ec = co_await connection_.async_write_message(req);
    if (ec) {
        pending_.erase(sb);
        co_return std::pair{ec, Message{}};
    }

    ec = co_await pending->ready.async_wait(timeout);
    if (ec == core::make_error_code(core::errc::timeout)) {
        // 控制事务超时（T6）：只返回 timeout，不在此处强制断线。
        //
        // 说明：
        // - SELECT 等握手失败是否“立即断线”属于更高层的策略（见 async_open_*）。
        // - LINKTEST 周期心跳通常需要“连续失败阈值”，因此也不能在这里一刀切断线。
        pending_.erase(sb);
        co_return std::pair{ec, Message{}};
    }

    pending_.erase(sb);
    if (ec) {
        co_return std::pair{pending->ec ? pending->ec : ec, Message{}};
    }
    if (pending->ec) {
        co_return std::pair{pending->ec, Message{}};
    }
    if (!pending->response.has_value()) {
        co_return std::pair{core::make_error_code(core::errc::invalid_argument),
                            Message{}};
    }

    co_return std::pair{std::error_code{}, *pending->response};
}

asio::awaitable<std::pair<std::error_code, Message>>
Session::async_data_transaction_(const Message &req, core::duration timeout) {
    // 数据事务（W=1）：同样用 pending_ 做请求-响应匹配；按 HSMS-SS 语义，
    // T3 超时只取消事务，不强制断线。
    const auto max_pending =
        options_.max_pending_requests == 0 ? std::size_t{1}
                                           : options_.max_pending_requests;
    if (pending_.size() >= max_pending) {
        co_return std::pair{core::make_error_code(core::errc::buffer_overflow),
                            Message{}};
    }

    const auto sb = req.header.system_bytes;
    auto pending = std::make_shared<Pending>(SType::data);
    pending_.insert_or_assign(sb, pending);

    auto ec = co_await connection_.async_write_message(req);
    if (ec) {
        pending_.erase(sb);
        co_return std::pair{ec, Message{}};
    }

    ec = co_await pending->ready.async_wait(timeout);
    if (ec == core::make_error_code(core::errc::timeout)) {
        pending_.erase(sb);
        co_return std::pair{ec, Message{}};
    }

    pending_.erase(sb);
    if (ec) {
        co_return std::pair{pending->ec ? pending->ec : ec, Message{}};
    }
    if (pending->ec) {
        co_return std::pair{pending->ec, Message{}};
    }
    if (!pending->response.has_value()) {
        co_return std::pair{core::make_error_code(core::errc::invalid_argument),
                            Message{}};
    }

    co_return std::pair{std::error_code{}, *pending->response};
}

asio::awaitable<std::error_code>
Session::async_open_active(const asio::ip::tcp::endpoint &endpoint) {
    if (stop_requested_) {
        co_return core::make_error_code(core::errc::cancelled);
    }

    SPDLOG_DEBUG("hsms open_active: port={} session_id={}",
                 endpoint.port(),
                 options_.session_id);

    Connection conn(executor_, ConnectionOptions{.t8 = options_.t8});
    auto ec = co_await conn.async_connect(endpoint);
    if (ec) {
        on_disconnected_(ec);
        co_return ec;
    }

    co_return co_await async_open_active(std::move(conn));
}

asio::awaitable<std::error_code>
Session::async_open_active(Connection &&connection) {
    if (stop_requested_) {
        co_return core::make_error_code(core::errc::cancelled);
    }

    SPDLOG_DEBUG("hsms open_active(connection): session_id={}", options_.session_id);

    if (reader_running_) {
        // 若旧连接的 reader_loop_ 仍在跑，先关闭旧连接并等待其退出，避免两个
        // reader 同时读不同连接造成状态混乱。
        (void)co_await connection_.async_close();
        (void)co_await disconnected_event_.async_wait(options_.t6);
    }

    connection_ = std::move(connection);
    reset_state_();

    start_reader_();

    Message req =
        make_select_req(kHsmsSsControlSessionId, allocate_system_bytes());
    auto [tr_ec, rsp] = co_await async_control_transaction_(
        req, SType::select_rsp, options_.t6);
    if (tr_ec) {
        // SELECT 控制事务超时（T6）按“通信失败”处理：断线收敛，避免双方状态机分叉。
        connection_.cancel_and_close();
        on_disconnected_(tr_ec);
        co_return tr_ec;
    }
    if (rsp.header.header_byte2 != kRspOk) {
        (void)co_await connection_.async_close();
        on_disconnected_(core::make_error_code(core::errc::invalid_argument));
        co_return core::make_error_code(core::errc::invalid_argument);
    }

    set_selected_();
    SPDLOG_DEBUG("hsms open_active selected");
    co_return std::error_code{};
}

asio::awaitable<std::error_code>
Session::async_open_passive(asio::ip::tcp::socket socket) {
    if (stop_requested_) {
        co_return core::make_error_code(core::errc::cancelled);
    }

    SPDLOG_DEBUG("hsms open_passive(socket): session_id={}", options_.session_id);

    Connection conn(std::move(socket), ConnectionOptions{.t8 = options_.t8});
    co_return co_await async_open_passive(std::move(conn));
}

asio::awaitable<std::error_code>
Session::async_open_passive(Connection &&connection) {
    if (stop_requested_) {
        co_return core::make_error_code(core::errc::cancelled);
    }

    SPDLOG_DEBUG("hsms open_passive(connection): session_id={}", options_.session_id);

    if (reader_running_) {
        // 同主动端：确保不会有“两个 reader_loop_ 同时存在”。
        (void)co_await connection_.async_close();
        (void)co_await disconnected_event_.async_wait(options_.t6);
    }

    connection_ = std::move(connection);
    reset_state_();
    start_reader_();

    auto ec = co_await selected_event_.async_wait(options_.t7);
    if (ec) {
        (void)co_await connection_.async_close();
        on_disconnected_(ec);
        co_return ec;
    }

    SPDLOG_DEBUG("hsms open_passive selected");
    co_return std::error_code{};
}

asio::awaitable<std::error_code>
Session::async_run_active(const asio::ip::tcp::endpoint &endpoint) {
    while (!stop_requested_) {
        auto ec = co_await async_open_active(endpoint);
        if (ec) {
            if (!options_.auto_reconnect || stop_requested_) {
                co_return ec;
            }
            // 连接失败：按 T5 退避后重试。
            Timer timer(executor_);
            (void)co_await timer.async_wait_for(options_.t5);
            continue;
        }

        // 等待断线，然后按 T5 退避后重连。
        (void)co_await disconnected_event_.async_wait(std::nullopt);
        if (!options_.auto_reconnect || stop_requested_) {
            co_return std::error_code{};
        }

        Timer timer(executor_);
        (void)co_await timer.async_wait_for(options_.t5);
    }
    co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::async_send(const Message &msg) {
    if (!connection_.is_open()) {
        co_return core::make_error_code(core::errc::invalid_argument);
    }
    if (msg.is_data() && state_ != SessionState::selected) {
        co_return core::make_error_code(core::errc::invalid_argument);
    }

    if (msg.is_data()) {
        SPDLOG_DEBUG("hsms send data: S{}F{} W={} sb={} body_n={}",
                     static_cast<int>(msg.stream()),
                     static_cast<int>(msg.function()),
                     msg.w_bit() ? 1 : 0,
                     msg.header.system_bytes,
                     msg.body.size());
    } else {
        SPDLOG_DEBUG("hsms send control: stype={} sb={}",
                     static_cast<int>(msg.header.s_type),
                     msg.header.system_bytes);
    }

    co_return co_await connection_.async_write_message(msg);
}

asio::awaitable<std::pair<std::error_code, Message>>
Session::async_receive_data(std::optional<core::duration> timeout) {
    while (inbound_data_.empty()) {
        auto ec = co_await inbound_event_.async_wait(timeout);
        if (ec) {
            co_return std::pair{ec, Message{}};
        }
    }

    Message msg = std::move(inbound_data_.front());
    inbound_data_.pop_front();
    if (inbound_data_.empty()) {
        inbound_event_.reset();
    }
    co_return std::pair{std::error_code{}, std::move(msg)};
}

asio::awaitable<std::pair<std::error_code, Message>>
Session::async_request_data(std::uint8_t stream,
                            std::uint8_t function,
                            core::bytes_view body) {
    co_return co_await async_request_data(stream, function, body, std::nullopt);
}

asio::awaitable<std::pair<std::error_code, Message>>
Session::async_request_data(std::uint8_t stream,
                            std::uint8_t function,
                            core::bytes_view body,
                            std::optional<core::duration> timeout) {
    if (state_ != SessionState::selected) {
        co_return std::pair{core::make_error_code(core::errc::invalid_argument),
                            Message{}};
    }
    if ((options_.session_id & 0x8000U) != 0) {
        // HSMS-SS：data message 的 SessionID 高位必须为 0（低 15 位为 DeviceID）。
        co_return std::pair{core::make_error_code(core::errc::invalid_argument),
                            Message{}};
    }

    const auto sb = allocate_system_bytes();
    Message req = make_data_message(
        options_.session_id, stream, function, true, sb, body);
    co_return co_await async_data_transaction_(req,
                                               timeout.value_or(options_.t3));
}

asio::awaitable<std::error_code> Session::async_linktest() {
    if (state_ != SessionState::selected) {
        co_return core::make_error_code(core::errc::invalid_argument);
    }

    Message req =
        make_linktest_req(kHsmsSsControlSessionId, allocate_system_bytes());
    auto [ec, rsp] = co_await async_control_transaction_(
        req, SType::linktest_rsp, options_.t6);
    if (ec) {
        co_return ec;
    }
    if (rsp.header.session_id != kHsmsSsControlSessionId) {
        co_return core::make_error_code(core::errc::invalid_argument);
    }
    co_return std::error_code{};
}

asio::awaitable<std::error_code>
Session::async_wait_selected(std::uint64_t min_generation,
                             core::duration timeout) {
    const auto deadline = core::steady_clock::now() + timeout;
    while (!stop_requested_) {
        if (state_ == SessionState::selected &&
            selected_generation_.load() >= min_generation) {
            co_return std::error_code{};
        }

        const auto now = core::steady_clock::now();
        if (now >= deadline) {
            co_return core::make_error_code(core::errc::timeout);
        }

        const auto remaining = deadline - now;
        auto ec = co_await selected_event_.async_wait(remaining);
        if (ec == core::make_error_code(core::errc::timeout)) {
            co_return ec;
        }
        if (ec && ec != core::make_error_code(core::errc::cancelled)) {
            co_return ec;
        }
    }
    co_return core::make_error_code(core::errc::cancelled);
}

} // namespace secs::hsms
