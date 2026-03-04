#pragma once

#include "secs/c_api.h"

#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/core/log.hpp"
#include "secs/core/metrics.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/ceid_dispatcher.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/block.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"

#include <asio/detail/config.hpp>
#if defined(ASIO_HAS_SERIAL_PORT)
#include "secs/secs1/serial_port_link.hpp"
#endif

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/*
 * C API（C ABI）实现文件。
 *
 * 本文件实现 `include/secs/c_api.h` 中声明的 C 语言接口，核心目标是让纯 C 工程
 * 也能调用本库（底层实现仍为 C++20 + asio 协程）。
 *
 * 设计思路：
 * - 对外暴露的不透明句柄（opaque handle）在本文件中定义为真实的 C++ struct；
 * - 每个 `secs_context` 内部维护一个 io_context + 1 个 io 线程；
 * - “阻塞式 API”通过 `run_blocking()` 把协程投递到 io 线程执行，调用线程用
 *   条件变量等待结果返回。
 *
 * 错误与内存约定：
 * - 错误统一用 `secs_error_t{value, category}` 表达，对应 C++ 的 std::error_code；
 * - 跨 ABI 返回的堆内存统一使用 `secs_malloc/secs_free`（malloc/free），避免跨
 *   CRT/运行时导致的释放不匹配；
 * - C++ 异常禁止跨越 C 边界：内部捕获并映射到 `secs.c_api` 错误域。
 *
 * 线程与并发注意：
 * - 阻塞式 API 禁止在 io 线程调用，否则会形成死锁，故检测并返回 WRONG_THREAD；
 * - stop() 可能跨线程调用，内部通过 post 收敛到 io 线程执行，因此会话对象使用
 *   shared_ptr 以避免“stop 已投递但对象已销毁”的悬空访问。
 */

// -----------------------------------------------------------------------------
// 不透明句柄的真实定义（只在 C++ 实现文件内可见）
// -----------------------------------------------------------------------------

struct secs_context final {
    std::atomic_size_t ref_count{1};
    std::atomic_bool destroyed{false};
    asio::io_context ioc{};
    asio::executor_work_guard<asio::io_context::executor_type> work{
        asio::make_work_guard(ioc)};
    std::vector<std::thread> io_threads{};
    std::vector<std::thread::id> io_thread_ids{};
};

[[maybe_unused]] inline void context_retain(secs_context *ctx) noexcept {
    if (!ctx) {
        return;
    }
    (void)ctx->ref_count.fetch_add(1, std::memory_order_relaxed);
}

[[maybe_unused]] inline void context_release(secs_context *ctx) noexcept {
    if (!ctx) {
        return;
    }
    if (ctx->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete ctx;
    }
}

[[maybe_unused]] [[nodiscard]] inline bool
context_is_alive(const secs_context *ctx) noexcept {
    if (!ctx) {
        return false;
    }
    return !ctx->destroyed.load(std::memory_order_acquire);
}

struct secs_ii_item final {
    explicit secs_ii_item(secs::ii::Item v) : item(std::move(v)) {}
    secs::ii::Item item;
};

struct secs_ii_builder final {
    // 尚未闭合的 List 栈（每一层都是一个独立的 List Item）。
    std::vector<secs::ii::Item> list_stack{};

    // 已构建完成的 root（当 list_stack 为空时，add_* 或 list_end 会生成它）。
    std::optional<secs::ii::Item> root{};

    // 首个错误（记忆首错；后续调用直接返回该错误且不再修改状态）。
    secs_error_t first_err{0, "secs.c_api"};

    // finalize 成功后置为 true（防止误复用）。
    bool finalized{false};
};

struct secs_sml_runtime final {
    secs::sml::Runtime rt{};
};

struct secs_sml_render_context final {
    secs::sml::RenderContext ctx{};
    bool sticky_enabled{false};
    secs_error_t sticky_err{0, "secs.c_api"};
};

struct secs_hsms_connection final {
    explicit secs_hsms_connection(secs::hsms::Connection v)
        : conn(std::move(v)) {}
    secs::hsms::Connection conn;
};

struct secs_hsms_session final {
    struct context_ref final {
        secs_context *ptr{nullptr};
        ~context_ref() noexcept { context_release(ptr); }
    };

    // 必须放在首位：确保在其他成员（尤其 sess）析构后才释放 context。
    context_ref ctx_ref{};
    secs_context *ctx{nullptr};
    secs::hsms::SessionOptions options{};
    // 用 shared_ptr 的原因：
    // - stop() 允许跨线程调用（内部通过 post 到 io 线程）
    // - 必须避免 stop() 先 post、再立刻 destroy() 导致回调访问已释放对象（UAF）
    std::shared_ptr<secs::hsms::Session> sess{};
};

struct protocol_state final {
    struct context_ref final {
        secs_context *ptr{nullptr};
        ~context_ref() noexcept { context_release(ptr); }
    };

    // 必须放在首位：确保在其他成员（尤其 sess）析构后才释放 context。
    context_ref ctx_ref{};
    secs_context *ctx{nullptr};
    // 保证底层 HSMS 会话在 protocol::Session 存活期间不会被提前释放（避免
    // UAF）。
    std::shared_ptr<secs::hsms::Session> hsms_keepalive{};
    // SECS-I：底层 Link/StateMachine 需要在 protocol::Session 存活期间保持有效。
    std::unique_ptr<secs::secs1::Link> secs1_link{};
    std::unique_ptr<secs::secs1::StateMachine> secs1_sm{};
    std::unique_ptr<secs::protocol::Session> sess{};
    secs::core::Event run_done{};
    std::atomic_bool run_spawned{false};

    // runtime dump sink（可选）：由 C 侧传入回调，用于接收 dump 字符串。
    secs_protocol_dump_sink_fn dump_sink{nullptr};
    void *dump_sink_user{nullptr};
};

struct secs_protocol_session final {
    std::shared_ptr<protocol_state> state{};
};

struct secs_ceid_dispatcher final {
    std::shared_ptr<secs::protocol::CeidDispatcher> dispatcher{};
};

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

constexpr const char *kCApiCategory = "secs.c_api";

[[maybe_unused]] [[nodiscard]] secs_error_t ok() noexcept {
    return secs_error_t{0, kCApiCategory};
}

[[maybe_unused]] [[nodiscard]] secs_error_t c_api_err(secs_c_api_errc_t code) noexcept {
    return secs_error_t{static_cast<int>(code), kCApiCategory};
}

[[maybe_unused]] [[nodiscard]] secs_error_t from_error_code(const std::error_code &ec) noexcept {
    if (!ec) {
        return ok();
    }
    return secs_error_t{ec.value(), ec.category().name()};
}

[[maybe_unused]] [[nodiscard]] const std::error_category *
category_from_name(const char *name) noexcept {
    if (name == nullptr) {
        return nullptr;
    }

    // 本库自定义错误域
    if (std::strcmp(name, secs::core::error_category().name()) == 0) {
        return &secs::core::error_category();
    }
    if (std::strcmp(name, secs::secs1::error_category().name()) == 0) {
        return &secs::secs1::error_category();
    }
    if (std::strcmp(name, secs::ii::error_category().name()) == 0) {
        return &secs::ii::error_category();
    }
    if (std::strcmp(name, secs::sml::lexer_error_category().name()) == 0) {
        return &secs::sml::lexer_error_category();
    }
    if (std::strcmp(name, secs::sml::parser_error_category().name()) == 0) {
        return &secs::sml::parser_error_category();
    }
    if (std::strcmp(name, secs::sml::render_error_category().name()) == 0) {
        return &secs::sml::render_error_category();
    }

    // 标准错误域
    if (std::strcmp(name, std::system_category().name()) == 0) {
        return &std::system_category();
    }
    if (std::strcmp(name, std::generic_category().name()) == 0) {
        return &std::generic_category();
    }

    return nullptr;
}

[[maybe_unused]] [[nodiscard]] std::string c_api_message_for(int value) {
    switch (static_cast<secs_c_api_errc_t>(value)) {
    case SECS_C_API_OK:
        return "ok";
    case SECS_C_API_INVALID_ARGUMENT:
        return "invalid argument";
    case SECS_C_API_NOT_FOUND:
        return "not found";
    case SECS_C_API_OUT_OF_MEMORY:
        return "out of memory";
    case SECS_C_API_WRONG_THREAD:
        return "wrong thread (blocking API called from io thread)";
    case SECS_C_API_EXCEPTION:
        return "exception caught inside C API";
    }
    return "unknown secs.c_api error";
}

[[maybe_unused]] [[nodiscard]] char *dup_string(const std::string &s) noexcept {
    // 返回给 C 的字符串统一走 malloc/free，避免跨 CRT 问题。
    auto *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

[[maybe_unused]] [[nodiscard]] bool is_io_thread(const secs_context *ctx) noexcept {
    if (!context_is_alive(ctx)) {
        return false;
    }
    const auto self = std::this_thread::get_id();
    for (const auto tid : ctx->io_thread_ids) {
        if (tid == self) {
            return true;
        }
    }
    return false;
}

template <class Result, class AwaitableFactory>
[[maybe_unused]] asio::awaitable<Result>
run_blocking_invoke_factory(std::shared_ptr<AwaitableFactory> factory) {
    // 注意：不要直接使用“捕获变量的协程 lambda”作为 co_spawn(awaitable)
    // 的入参，否则闭包对象可能先于协程恢复而析构，触发悬空访问。
    // 这里把 factory 放进 shared_ptr，并作为协程参数持有到结束。
    co_return co_await (*factory)();
}

template <class Result, class AwaitableFactory>
[[maybe_unused]] secs_error_t run_blocking(secs_context *ctx,
                                          AwaitableFactory &&make_awaitable,
                                          Result &out) {
    static constexpr const char *kCalls = "secs.c_api.run_blocking.calls";
    static constexpr const char *kOk = "secs.c_api.run_blocking.ok";
    static constexpr const char *kErrors = "secs.c_api.run_blocking.errors";
    static constexpr const char *kWrongThread =
        "secs.c_api.run_blocking.wrong_thread";
    static constexpr const char *kWaitMs = "secs.c_api.run_blocking.wait_ms";

    secs::core::metrics_counter(kCalls, 1);

    if (!ctx) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }

    // 在整个阻塞调用期间持有 context 引用，避免并发 destroy 导致悬空指针。
    context_retain(ctx);
    struct context_hold final {
        secs_context *ctx{nullptr};
        ~context_hold() noexcept { context_release(ctx); }
    } hold{ctx};

    if (!context_is_alive(ctx)) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (is_io_thread(ctx)) {
        secs::core::metrics_counter(kErrors, 1);
        secs::core::metrics_counter(kWrongThread, 1);
        return c_api_err(SECS_C_API_WRONG_THREAD);
    }

    struct wait_state final {
        std::mutex mu{};
        std::condition_variable cv{};
        bool done{false};
        std::exception_ptr eptr{};
        std::optional<Result> result{};
    };
    auto state = std::make_shared<wait_state>();
    using factory_t = std::decay_t<AwaitableFactory>;
    std::shared_ptr<factory_t> factory;
    try {
        factory = std::make_shared<factory_t>(
            std::forward<AwaitableFactory>(make_awaitable));
    } catch (const std::bad_alloc &) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    } catch (...) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_EXCEPTION);
    }

    const auto start = std::chrono::steady_clock::now();

    asio::co_spawn(
        ctx->ioc,
        run_blocking_invoke_factory<Result, factory_t>(std::move(factory)),
        [state](std::exception_ptr e, Result r) {
            {
                std::lock_guard lk(state->mu);
                state->eptr = e;
                state->result = std::move(r);
                state->done = true;
            }
            state->cv.notify_one();
        });

    std::unique_lock lk(state->mu);
    while (!state->done) {
        if (!context_is_alive(ctx)) {
            secs::core::metrics_counter(kErrors, 1);
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        state->cv.wait_for(lk, std::chrono::milliseconds(10));
    }

    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    if (waited_ms >= 0) {
        secs::core::metrics_histogram(kWaitMs,
                                      static_cast<std::uint64_t>(waited_ms));
    }

    if (state->eptr) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_EXCEPTION);
    }
    if (!state->result.has_value()) {
        secs::core::metrics_counter(kErrors, 1);
        return c_api_err(SECS_C_API_EXCEPTION);
    }

    out = std::move(*state->result);
    secs::core::metrics_counter(kOk, 1);
    return ok();
}

template <class AwaitableFactory>
[[maybe_unused]] secs_error_t run_blocking_ec(secs_context *ctx,
                                             AwaitableFactory &&make_awaitable) {
    std::error_code ec{};
    auto bridge = run_blocking<std::error_code>(
        ctx, std::forward<AwaitableFactory>(make_awaitable), ec);
    if (!secs_error_is_ok(bridge)) {
        return bridge;
    }
    return from_error_code(ec);
}

template <class Fn>
[[maybe_unused]] secs_error_t guard_error(Fn &&fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    } catch (...) {
        return c_api_err(SECS_C_API_EXCEPTION);
    }
}

template <class Fn>
[[maybe_unused]] void guard_void(Fn &&fn) noexcept {
    try {
        fn();
    } catch (...) {
        // C ABI 边界禁止异常跨越；这里选择吞掉异常，避免 UB。
    }
}

// --------------------------------------------------------------------------
// HSMS 内存连接：用于测试/无 socket 环境
// --------------------------------------------------------------------------

struct MemoryChannel final {
    std::mutex mu{};
    std::deque<byte> buf{};
    bool closed{false};
    secs::core::Event data_event{};
};

class MemoryStream final : public secs::hsms::Stream {
public:
    MemoryStream(asio::any_io_executor ex,
                 std::shared_ptr<MemoryChannel> inbox,
                 std::shared_ptr<MemoryChannel> outbox)
        : ex_(ex), inbox_(std::move(inbox)), outbox_(std::move(outbox)) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }

    void cancel() noexcept override {
        if (!inbox_) {
            return;
        }
        inbox_->data_event.cancel();
    }

    void close() noexcept override {
        if (!open_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (outbox_) {
            {
                std::lock_guard lk(outbox_->mu);
                outbox_->closed = true;
            }
            outbox_->data_event.set();
        }
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(secs::core::mutable_bytes_view dst) override {
        if (!inbox_) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                std::size_t{0}};
        }

        for (;;) {
            bool need_wait = false;
            std::size_t n = 0;

            {
                std::lock_guard lk(inbox_->mu);
                if (!inbox_->buf.empty()) {
                    n = std::min(dst.size(), inbox_->buf.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        dst[i] = inbox_->buf.front();
                        inbox_->buf.pop_front();
                    }
                    if (inbox_->buf.empty()) {
                        // 与队列状态同锁更新，避免“写方 set 与读方 reset 交错”导致丢唤醒。
                        inbox_->data_event.reset();
                    }
                } else if (inbox_->closed) {
                    co_return std::pair{
                        std::make_error_code(std::errc::broken_pipe),
                        std::size_t{0}};
                } else {
                    need_wait = true;
                }
            }

            if (!need_wait) {
                co_return std::pair{std::error_code{}, n};
            }

            auto wait_ec = co_await inbox_->data_event.async_wait(std::nullopt);
            if (wait_ec) {
                co_return std::pair{wait_ec, std::size_t{0}};
            }
        }
    }

    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        if (!open_.load(std::memory_order_acquire)) {
            co_return make_error_code(errc::cancelled);
        }
        if (!outbox_) {
            co_return make_error_code(errc::invalid_argument);
        }
        {
            std::lock_guard lk(outbox_->mu);
            outbox_->buf.insert(outbox_->buf.end(), src.begin(), src.end());
        }
        if (!src.empty()) {
            outbox_->data_event.set();
        }
        co_return std::error_code{};
    }

    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }

private:
    asio::any_io_executor ex_;
    std::shared_ptr<MemoryChannel> inbox_;
    std::shared_ptr<MemoryChannel> outbox_;
    std::atomic_bool open_{true};
};

[[maybe_unused]] [[nodiscard]] secs::core::duration
ms_to_duration_or_default(std::uint32_t ms, secs::core::duration def) {
    if (ms == 0) {
        return def;
    }
    return std::chrono::milliseconds(ms);
}

[[maybe_unused]] [[nodiscard]] std::optional<secs::core::duration>
ms_to_optional_duration(std::uint32_t ms) {
    if (ms == 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(ms);
}

[[maybe_unused]] [[nodiscard]] std::vector<byte> bytes_to_vec(const uint8_t *p,
                                                              size_t n) {
    if (!p || n == 0) {
        return {};
    }
    std::vector<byte> out;
    out.resize(n);
    std::memcpy(out.data(), p, n);
    return out;
}

[[maybe_unused]] secs_error_t
fill_hsms_out_message(const secs::hsms::Message &msg,
                      secs_hsms_data_message_t *out) noexcept {
    if (!out) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    secs_hsms_data_message_free(out);

    out->session_id = msg.header.session_id;
    out->stream = msg.stream();
    out->function = msg.function();
    out->w_bit = msg.w_bit() ? 1 : 0;
    out->system_bytes = msg.header.system_bytes;

    if (msg.body.empty()) {
        out->body = nullptr;
        out->body_n = 0;
        return ok();
    }

    auto *buf = static_cast<uint8_t *>(secs_malloc(msg.body.size()));
    if (!buf) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    }
    std::memcpy(buf, msg.body.data(), msg.body.size());
    out->body = buf;
    out->body_n = msg.body.size();
    return ok();
}

[[maybe_unused]] secs_error_t
fill_protocol_out_message(const secs::protocol::DataMessage &msg,
                          secs_data_message_t *out) noexcept {
    if (!out) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    secs_data_message_free(out);

    out->stream = msg.stream;
    out->function = msg.function;
    out->w_bit = msg.w_bit ? 1 : 0;
    out->system_bytes = msg.system_bytes;

    if (msg.body.empty()) {
        out->body = nullptr;
        out->body_n = 0;
        return ok();
    }

    auto *buf = static_cast<uint8_t *>(secs_malloc(msg.body.size()));
    if (!buf) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    }
    std::memcpy(buf, msg.body.data(), msg.body.size());
    out->body = buf;
    out->body_n = msg.body.size();
    return ok();
}

[[maybe_unused]] [[nodiscard]] secs::ii::DecodeLimits
make_decode_limits(const secs_ii_decode_limits_t *limits) noexcept {
    secs::ii::DecodeLimits out{};
    if (!limits) {
        return out;
    }

    // 约定：0 表示“使用库默认值”，便于调用方 memset(0) 后仅覆盖少数字段。
    if (limits->max_depth != 0) {
        out.max_depth = limits->max_depth;
    }
    if (limits->max_list_items != 0) {
        out.max_list_items = limits->max_list_items;
    }
    if (limits->max_payload_bytes != 0) {
        out.max_payload_bytes = limits->max_payload_bytes;
    }
    if (limits->max_total_items != 0) {
        out.max_total_items = limits->max_total_items;
    }
    if (limits->max_total_bytes != 0) {
        out.max_total_bytes = limits->max_total_bytes;
    }
    return out;
}

[[maybe_unused]] [[nodiscard]] std::optional<std::uint32_t>
extract_u32_scalar(const secs::ii::Item &item) noexcept {
    if (auto *u1 = item.get_if<secs::ii::U1>()) {
        if (u1->values.size() == 1) {
            return u1->values[0];
        }
        return std::nullopt;
    }
    if (auto *u2 = item.get_if<secs::ii::U2>()) {
        if (u2->values.size() == 1) {
            return u2->values[0];
        }
        return std::nullopt;
    }
    if (auto *u4 = item.get_if<secs::ii::U4>()) {
        if (u4->values.size() == 1) {
            return u4->values[0];
        }
        return std::nullopt;
    }
    if (auto *u8 = item.get_if<secs::ii::U8>()) {
        if (u8->values.size() == 1) {
            const auto v = u8->values[0];
            if (v <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::uint32_t>::max())) {
                return static_cast<std::uint32_t>(v);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

[[maybe_unused]] [[nodiscard]] std::optional<std::uint32_t>
extract_u32_from_list_path(const secs::ii::Item &root,
                           const std::vector<std::size_t> &path) noexcept {
    const secs::ii::Item *cur = &root;
    for (const auto idx : path) {
        auto *list = cur->get_if<secs::ii::List>();
        if (!list || idx >= list->size()) {
            return std::nullopt;
        }
        cur = &(*list)[idx];
    }
    return extract_u32_scalar(*cur);
}

[[maybe_unused]] [[nodiscard]] secs::hsms::SessionOptions
make_hsms_options(const secs_hsms_session_options_t *options) {
    secs::hsms::SessionOptions opt{};
    opt.session_id = options->session_id;
    opt.t3 = ms_to_duration_or_default(options->t3_ms, opt.t3);
    opt.t5 = ms_to_duration_or_default(options->t5_ms, opt.t5);
    opt.t6 = ms_to_duration_or_default(options->t6_ms, opt.t6);
    opt.t7 = ms_to_duration_or_default(options->t7_ms, opt.t7);
    opt.t8 = ms_to_duration_or_default(options->t8_ms, opt.t8);
    opt.linktest_interval = ms_to_duration_or_default(options->linktest_interval_ms,
                                                      secs::core::duration{});

    if (options->linktest_max_consecutive_failures != 0) {
        opt.linktest_max_consecutive_failures = std::max<std::uint32_t>(
            1U, options->linktest_max_consecutive_failures);
    }

    opt.auto_reconnect = options->auto_reconnect != 0;
    opt.passive_accept_select = options->passive_accept_select != 0;
    return opt;
}

} // namespace
