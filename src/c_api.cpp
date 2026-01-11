#include "secs/c_api.h"

#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/core/log.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/block.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"

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
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
    asio::io_context ioc{};
    asio::executor_work_guard<asio::io_context::executor_type> work{
        asio::make_work_guard(ioc)};
    std::vector<std::thread> io_threads{};
    std::vector<std::thread::id> io_thread_ids{};
};

struct secs_ii_item final {
    explicit secs_ii_item(secs::ii::Item v) : item(std::move(v)) {}
    secs::ii::Item item;
};

struct secs_sml_runtime final {
    secs::sml::Runtime rt{};
};

struct secs_hsms_connection final {
    explicit secs_hsms_connection(secs::hsms::Connection v)
        : conn(std::move(v)) {}
    secs::hsms::Connection conn;
};

struct secs_hsms_session final {
    secs_context *ctx{nullptr};
    secs::hsms::SessionOptions options{};
    // 用 shared_ptr 的原因：
    // - stop() 允许跨线程调用（内部通过 post 到 io 线程）
    // - 必须避免 stop() 先 post、再立刻 destroy() 导致回调访问已释放对象（UAF）
    std::shared_ptr<secs::hsms::Session> sess{};
};

struct protocol_state final {
    secs_context *ctx{nullptr};
    // 保证底层 HSMS 会话在 protocol::Session 存活期间不会被提前释放（避免
    // UAF）。
    std::shared_ptr<secs::hsms::Session> hsms_keepalive{};
    std::unique_ptr<secs::protocol::Session> sess{};
    secs::core::Event run_done{};

    // runtime dump sink（可选）：由 C 侧传入回调，用于接收 dump 字符串。
    secs_protocol_dump_sink_fn dump_sink{nullptr};
    void *dump_sink_user{nullptr};
};

struct secs_protocol_session final {
    std::shared_ptr<protocol_state> state{};
};

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

constexpr const char *kCApiCategory = "secs.c_api";

[[nodiscard]] secs_error_t ok() noexcept {
    return secs_error_t{0, kCApiCategory};
}

[[nodiscard]] secs_error_t c_api_err(secs_c_api_errc_t code) noexcept {
    return secs_error_t{static_cast<int>(code), kCApiCategory};
}

[[nodiscard]] secs_error_t from_error_code(const std::error_code &ec) noexcept {
    if (!ec) {
        return ok();
    }
    return secs_error_t{ec.value(), ec.category().name()};
}

[[nodiscard]] const std::error_category *
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

[[nodiscard]] std::string c_api_message_for(int value) {
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

[[nodiscard]] char *dup_string(const std::string &s) noexcept {
    // 返回给 C 的字符串统一走 malloc/free，避免跨 CRT 问题。
    auto *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

[[nodiscard]] bool is_io_thread(const secs_context *ctx) noexcept {
    if (!ctx) {
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
secs_error_t run_blocking(secs_context *ctx,
                          AwaitableFactory &&make_awaitable,
                          Result &out) {
    if (!ctx) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (is_io_thread(ctx)) {
        return c_api_err(SECS_C_API_WRONG_THREAD);
    }

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr eptr;
    std::optional<Result> result;

    asio::co_spawn(
        ctx->ioc, make_awaitable(), [&](std::exception_ptr e, Result r) {
            {
                std::lock_guard lk(mu);
                eptr = e;
                result = std::move(r);
                done = true;
            }
            cv.notify_one();
        });

    std::unique_lock lk(mu);
    cv.wait(lk, [&] { return done; });

    if (eptr) {
        return c_api_err(SECS_C_API_EXCEPTION);
    }
    if (!result.has_value()) {
        return c_api_err(SECS_C_API_EXCEPTION);
    }

    out = std::move(*result);
    return ok();
}

template <class AwaitableFactory>
secs_error_t run_blocking_ec(secs_context *ctx,
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
secs_error_t guard_error(Fn &&fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    } catch (...) {
        return c_api_err(SECS_C_API_EXCEPTION);
    }
}

template <class Fn>
void guard_void(Fn &&fn) noexcept {
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
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    void cancel() noexcept override {
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    void close() noexcept override {
        if (!open_) {
            return;
        }
        open_ = false;
        if (outbox_) {
            outbox_->closed = true;
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

        while (inbox_->buf.empty()) {
            if (inbox_->closed) {
                co_return std::pair{
                    std::make_error_code(std::errc::broken_pipe),
                    std::size_t{0}};
            }
            auto ec = co_await inbox_->data_event.async_wait(std::nullopt);
            if (ec) {
                co_return std::pair{ec, std::size_t{0}};
            }
        }

        const std::size_t n = std::min(dst.size(), inbox_->buf.size());
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = inbox_->buf.front();
            inbox_->buf.pop_front();
        }
        if (inbox_->buf.empty()) {
            inbox_->data_event.reset();
        }

        co_return std::pair{std::error_code{}, n};
    }

    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        if (!open_) {
            co_return make_error_code(errc::cancelled);
        }
        if (!outbox_) {
            co_return make_error_code(errc::invalid_argument);
        }
        outbox_->buf.insert(outbox_->buf.end(), src.begin(), src.end());
        outbox_->data_event.set();
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
    bool open_{true};
};

[[nodiscard]] secs::core::duration
ms_to_duration_or_default(std::uint32_t ms, secs::core::duration def) {
    if (ms == 0) {
        return def;
    }
    return std::chrono::milliseconds(ms);
}

[[nodiscard]] std::optional<secs::core::duration>
ms_to_optional_duration(std::uint32_t ms) {
    if (ms == 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(ms);
}

[[nodiscard]] std::vector<byte> bytes_to_vec(const uint8_t *p, size_t n) {
    if (!p || n == 0) {
        return {};
    }
    std::vector<byte> out;
    out.resize(n);
    std::memcpy(out.data(), p, n);
    return out;
}

secs_error_t fill_hsms_out_message(const secs::hsms::Message &msg,
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

secs_error_t fill_protocol_out_message(const secs::protocol::DataMessage &msg,
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

[[nodiscard]] secs::ii::DecodeLimits
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

[[nodiscard]] secs::hsms::SessionOptions
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
    opt.auto_reconnect = options->auto_reconnect != 0;
    opt.passive_accept_select = options->passive_accept_select != 0;
    return opt;
}

[[nodiscard]] secs::hsms::SessionOptions
make_hsms_options_v2(const secs_hsms_session_options_v2_t *options) {
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
        opt.linktest_max_consecutive_failures =
            std::max<std::uint32_t>(1U, options->linktest_max_consecutive_failures);
    }

    opt.auto_reconnect = options->auto_reconnect != 0;
    opt.passive_accept_select = options->passive_accept_select != 0;
    return opt;
}

} // namespace

// ----------------------------- 内存/错误/版本 -----------------------------

void *secs_malloc(size_t n) { return std::malloc(n); }

void secs_free(void *p) { std::free(p); }

char *secs_error_message(secs_error_t err) {
    try {
        if (err.value == 0) {
            return dup_string("ok");
        }

        if (err.category && std::strcmp(err.category, kCApiCategory) == 0) {
            return dup_string(c_api_message_for(err.value));
        }

        const auto *cat = category_from_name(err.category);
        if (!cat) {
            std::string msg = "unknown error category";
            if (err.category) {
                msg += ": ";
                msg += err.category;
            }
            msg += " (";
            msg += std::to_string(err.value);
            msg += ")";
            return dup_string(msg);
        }

        return dup_string(std::error_code{err.value, *cat}.message());
    } catch (...) {
        return nullptr;
    }
}

const char *secs_version_string(void) {
#ifdef SECS_VERSION_STRING
    return SECS_VERSION_STRING;
#else
    return "0.1.0";
#endif
}

secs_error_t secs_log_set_level(secs_log_level_t level) {
    return guard_error([&]() -> secs_error_t {
        using secs::core::LogLevel;
        switch (level) {
        case SECS_LOG_TRACE:
            secs::core::set_log_level(LogLevel::trace);
            return ok();
        case SECS_LOG_DEBUG:
            secs::core::set_log_level(LogLevel::debug);
            return ok();
        case SECS_LOG_INFO:
            secs::core::set_log_level(LogLevel::info);
            return ok();
        case SECS_LOG_WARN:
            secs::core::set_log_level(LogLevel::warn);
            return ok();
        case SECS_LOG_ERROR:
            secs::core::set_log_level(LogLevel::error);
            return ok();
        case SECS_LOG_CRITICAL:
            secs::core::set_log_level(LogLevel::critical);
            return ok();
        case SECS_LOG_OFF:
            secs::core::set_log_level(LogLevel::off);
            return ok();
        }
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    });
}

// ----------------------------- 上下文 -----------------------------

secs_error_t secs_context_create_with_options(secs_context_t **out_ctx,
                                              const secs_context_options_t *opt) {
    return guard_error([&]() -> secs_error_t {
        if (!out_ctx) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ctx = nullptr;

        std::size_t io_threads = 1;
        if (opt && opt->io_threads != 0) {
            io_threads = opt->io_threads;
        }
        if (io_threads == 0) {
            io_threads = 1;
        }

        std::unique_ptr<secs_context> ctx(new (std::nothrow) secs_context{});
        if (!ctx) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        try {
            ctx->io_threads.reserve(io_threads);
            ctx->io_thread_ids.resize(io_threads);
        } catch (const std::bad_alloc &) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        } catch (...) {
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        std::vector<std::promise<void>> started;
        std::vector<std::future<void>> started_futures;
        try {
            started.resize(io_threads);
            started_futures.reserve(io_threads);
            for (std::size_t i = 0; i < io_threads; ++i) {
                started_futures.push_back(started[i].get_future());
            }
        } catch (const std::bad_alloc &) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        } catch (...) {
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        try {
            for (std::size_t i = 0; i < io_threads; ++i) {
                ctx->io_threads.emplace_back(
                    [raw = ctx.get(), i, p = std::move(started[i])]() mutable {
                        raw->io_thread_ids[i] = std::this_thread::get_id();
                        p.set_value();
                        raw->ioc.run();
                    });
            }
        } catch (...) {
            ctx->work.reset();
            ctx->ioc.stop();
            for (auto &t : ctx->io_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        for (auto &f : started_futures) {
            f.wait();
        }

        *out_ctx = ctx.release();
        return ok();
    });
}

secs_error_t secs_context_create(secs_context_t **out_ctx) {
    return secs_context_create_with_options(out_ctx, nullptr);
}

void secs_context_destroy(secs_context_t *ctx) {
    guard_void([&]() {
        if (!ctx) {
            return;
        }

        ctx->work.reset();
        ctx->ioc.stop();

        for (auto &t : ctx->io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        delete ctx;
    });
}

// ----------------------------- SECS-II：Item（数据项）
// -----------------------------

static secs_error_t new_item(secs::ii::Item v,
                             secs_ii_item_t **out_item) noexcept {
    if (!out_item) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_item = nullptr;

    auto *item = new (std::nothrow) secs_ii_item(std::move(v));
    if (!item) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    }
    *out_item = item;
    return ok();
}

secs_error_t secs_ii_item_create_list(secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        return new_item(secs::ii::Item::list({}), out_item);
    });
}

secs_error_t secs_ii_item_create_ascii(const char *bytes,
                                       size_t n,
                                       secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::string s;
        if (bytes && n != 0) {
            s.assign(bytes, bytes + n);
        }
        return new_item(secs::ii::Item::ascii(std::move(s)), out_item);
    });
}

secs_error_t secs_ii_item_create_binary(const uint8_t *bytes,
                                        size_t n,
                                        secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<byte> v = bytes_to_vec(bytes, n);
        return new_item(secs::ii::Item::binary(std::move(v)), out_item);
    });
}

secs_error_t secs_ii_item_create_boolean(const uint8_t *values01,
                                         size_t n,
                                         secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!values01 && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<bool> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(values01[i] != 0);
        }
        return new_item(secs::ii::Item::boolean(std::move(v)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i1(const int8_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        // C 语言调用约定：允许 v==NULL 且 n==0；此时必须构造空
        // vector，且不能做空指针算术。
        std::vector<std::int8_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i1(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i2(const int16_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int16_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i2(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i4(const int32_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int32_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i8(const int64_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int64_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i8(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u1(const uint8_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint8_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u1(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u2(const uint16_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint16_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u2(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u4(const uint32_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint32_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u8(const uint64_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint64_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u8(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_f4(const float *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<float> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::f4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_f8(const double *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<double> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::f8(std::move(out)), out_item);
    });
}

void secs_ii_item_destroy(secs_ii_item_t *item) {
    guard_void([&]() { delete item; });
}

secs_error_t secs_ii_item_get_type(const secs_ii_item_t *item,
                                   secs_ii_item_type_t *out_type) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_type) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        const auto &s = item->item.storage();
        if (std::holds_alternative<secs::ii::List>(s))
            *out_type = SECS_II_ITEM_LIST;
        else if (std::holds_alternative<secs::ii::ASCII>(s))
            *out_type = SECS_II_ITEM_ASCII;
        else if (std::holds_alternative<secs::ii::Binary>(s))
            *out_type = SECS_II_ITEM_BINARY;
        else if (std::holds_alternative<secs::ii::Boolean>(s))
            *out_type = SECS_II_ITEM_BOOLEAN;
        else if (std::holds_alternative<secs::ii::I1>(s))
            *out_type = SECS_II_ITEM_I1;
        else if (std::holds_alternative<secs::ii::I2>(s))
            *out_type = SECS_II_ITEM_I2;
        else if (std::holds_alternative<secs::ii::I4>(s))
            *out_type = SECS_II_ITEM_I4;
        else if (std::holds_alternative<secs::ii::I8>(s))
            *out_type = SECS_II_ITEM_I8;
        else if (std::holds_alternative<secs::ii::U1>(s))
            *out_type = SECS_II_ITEM_U1;
        else if (std::holds_alternative<secs::ii::U2>(s))
            *out_type = SECS_II_ITEM_U2;
        else if (std::holds_alternative<secs::ii::U4>(s))
            *out_type = SECS_II_ITEM_U4;
        else if (std::holds_alternative<secs::ii::U8>(s))
            *out_type = SECS_II_ITEM_U8;
        else if (std::holds_alternative<secs::ii::F4>(s))
            *out_type = SECS_II_ITEM_F4;
        else if (std::holds_alternative<secs::ii::F8>(s))
            *out_type = SECS_II_ITEM_F8;
        else
            return c_api_err(SECS_C_API_EXCEPTION);

        return ok();
    });
}

secs_error_t secs_ii_item_list_size(const secs_ii_item_t *item, size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *list = item->item.get_if<secs::ii::List>();
        if (!list)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_n = list->size();
        return ok();
    });
}

secs_error_t secs_ii_item_list_get(const secs_ii_item_t *item,
                                   size_t index,
                                   secs_ii_item_t **out_child) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_child)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_child = nullptr;
        const auto *list = item->item.get_if<secs::ii::List>();
        if (!list)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (index >= list->size())
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return new_item((*list)[index], out_child);
    });
}

secs_error_t secs_ii_item_list_append(secs_ii_item_t *list,
                                      const secs_ii_item_t *elem) {
    return guard_error([&]() -> secs_error_t {
        if (!list || !elem)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        auto *l = list->item.get_if<secs::ii::List>();
        if (!l)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        l->push_back(elem->item);
        return ok();
    });
}

secs_error_t secs_ii_item_ascii_view(const secs_ii_item_t *item,
                                     const char **out_ptr,
                                     size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_ptr || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *v = item->item.get_if<secs::ii::ASCII>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_ptr = v->value.data();
        *out_n = v->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_binary_view(const secs_ii_item_t *item,
                                      const uint8_t **out_ptr,
                                      size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_ptr || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *v = item->item.get_if<secs::ii::Binary>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_ptr = reinterpret_cast<const uint8_t *>(v->value.data());
        *out_n = v->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_boolean_copy(const secs_ii_item_t *item,
                                       uint8_t **out_values01,
                                       size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_values01 || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_values01 = nullptr;
        *out_n = 0;
        const auto *v = item->item.get_if<secs::ii::Boolean>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        if (v->values.empty()) {
            return ok();
        }
        auto *buf = static_cast<uint8_t *>(secs_malloc(v->values.size()));
        if (!buf)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        for (size_t i = 0; i < v->values.size(); ++i) {
            buf[i] = v->values[i] ? 1u : 0u;
        }
        *out_values01 = buf;
        *out_n = v->values.size();
        return ok();
    });
}

#define SECS_II_VIEW_IMPL(c_type, cpp_type, tag)                               \
    secs_error_t secs_ii_item_##tag##_view(                                    \
        const secs_ii_item_t *item, const c_type **out_ptr, size_t *out_n) {   \
        if (!item || !out_ptr || !out_n)                                       \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                     \
        const auto *v = item->item.get_if<cpp_type>();                         \
        if (!v)                                                                \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                     \
        *out_ptr = reinterpret_cast<const c_type *>(v->values.data());         \
        *out_n = v->values.size();                                             \
        return ok();                                                           \
    }

SECS_II_VIEW_IMPL(int8_t, secs::ii::I1, i1)
SECS_II_VIEW_IMPL(int16_t, secs::ii::I2, i2)
SECS_II_VIEW_IMPL(int32_t, secs::ii::I4, i4)
SECS_II_VIEW_IMPL(int64_t, secs::ii::I8, i8)
SECS_II_VIEW_IMPL(uint8_t, secs::ii::U1, u1)
SECS_II_VIEW_IMPL(uint16_t, secs::ii::U2, u2)
SECS_II_VIEW_IMPL(uint32_t, secs::ii::U4, u4)
SECS_II_VIEW_IMPL(uint64_t, secs::ii::U8, u8)
SECS_II_VIEW_IMPL(float, secs::ii::F4, f4)
SECS_II_VIEW_IMPL(double, secs::ii::F8, f8)

#undef SECS_II_VIEW_IMPL

secs_error_t
secs_ii_encode(const secs_ii_item_t *item, uint8_t **out_bytes, size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_bytes || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_bytes = nullptr;
        *out_n = 0;

        std::vector<byte> out;
        const auto ec = secs::ii::encode(item->item, out);
        if (ec) {
            return from_error_code(ec);
        }
        if (out.empty()) {
            return ok();
        }

        auto *buf = static_cast<uint8_t *>(secs_malloc(out.size()));
        if (!buf) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(buf, out.data(), out.size());
        *out_bytes = buf;
        *out_n = out.size();
        return ok();
    });
}

secs_error_t secs_ii_decode_one(const uint8_t *in_bytes,
                                size_t in_n,
                                size_t *out_consumed,
                                secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!in_bytes && in_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!out_consumed || !out_item)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_consumed = 0;
        *out_item = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            bytes_view{reinterpret_cast<const byte *>(in_bytes), in_n},
            decoded,
            consumed);
        if (ec) {
            return from_error_code(ec);
        }

        auto *h = new (std::nothrow) secs_ii_item(std::move(decoded));
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_consumed = consumed;
        *out_item = h;
        return ok();
    });
}

void secs_ii_decode_limits_init_default(secs_ii_decode_limits_t *out_limits) {
    guard_void([&]() {
        if (!out_limits) {
            return;
        }
        const secs::ii::DecodeLimits d{};
        out_limits->max_depth = d.max_depth;
        out_limits->max_list_items = d.max_list_items;
        out_limits->max_payload_bytes = d.max_payload_bytes;
        out_limits->max_total_items = d.max_total_items;
        out_limits->max_total_bytes = d.max_total_bytes;
    });
}

secs_error_t secs_ii_decode_one_with_limits(const uint8_t *in_bytes,
                                            size_t in_n,
                                            const secs_ii_decode_limits_t *limits,
                                            size_t *out_consumed,
                                            secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!in_bytes && in_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!out_consumed || !out_item) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_consumed = 0;
        *out_item = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            bytes_view{reinterpret_cast<const byte *>(in_bytes), in_n},
            decoded,
            consumed,
            make_decode_limits(limits));
        if (ec) {
            return from_error_code(ec);
        }

        auto *h = new (std::nothrow) secs_ii_item(std::move(decoded));
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_consumed = consumed;
        *out_item = h;
        return ok();
    });
}

// ----------------------------- SML 运行时 -----------------------------

secs_error_t secs_sml_runtime_create(secs_sml_runtime_t **out_rt) {
    return guard_error([&]() -> secs_error_t {
        if (!out_rt)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_rt = nullptr;

        auto *rt = new (std::nothrow) secs_sml_runtime{};
        if (!rt)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        *out_rt = rt;
        return ok();
    });
}

void secs_sml_runtime_destroy(secs_sml_runtime_t *rt) {
    guard_void([&]() { delete rt; });
}

secs_error_t secs_sml_runtime_load(secs_sml_runtime_t *rt,
                                   const char *source,
                                   size_t source_n) {
    return guard_error([&]() -> secs_error_t {
        if (!rt)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!source && source_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const std::string_view sv{source ? source : "", source ? source_n : 0};
        return from_error_code(rt->rt.load(sv));
    });
}

secs_error_t secs_sml_runtime_match_response(const secs_sml_runtime_t *rt,
                                             uint8_t stream,
                                             uint8_t function,
                                             const uint8_t *body_bytes,
                                             size_t body_n,
                                             char **out_name) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !out_name)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_name = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        auto dec_ec = secs::ii::decode_one(
            bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
            decoded,
            consumed);
        if (dec_ec) {
            return from_error_code(dec_ec);
        }

        auto matched = rt->rt.match_response(stream, function, decoded);
        if (!matched.has_value()) {
            return ok();
        }

        auto *s = static_cast<char *>(secs_malloc(matched->size() + 1));
        if (!s) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(s, matched->data(), matched->size());
        s[matched->size()] = '\0';
        *out_name = s;
        return ok();
    });
}

secs_error_t
secs_sml_runtime_get_message_body_by_name(const secs_sml_runtime_t *rt,
                                          const char *name,
                                          uint8_t **out_body_bytes,
                                          size_t *out_body_n,
                                          uint8_t *out_stream,
                                          uint8_t *out_function,
                                          int *out_w_bit) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !name || !out_body_bytes || !out_body_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_body_bytes = nullptr;
        *out_body_n = 0;

        const auto *msg = rt->rt.get_message(std::string_view{name});
        if (!msg) {
            return c_api_err(SECS_C_API_NOT_FOUND);
        }

        // C API 暂不提供“注入变量”的接口：这里使用空上下文渲染。
        // 若消息模板包含占位符，将返回 sml.render/missing_variable。
        secs::sml::RenderContext ctx{};
        secs::ii::Item rendered{secs::ii::List{}};
        const auto render_ec = secs::sml::render_item(msg->item, ctx, rendered);
        if (render_ec) {
            return from_error_code(render_ec);
        }

        std::vector<byte> out;
        auto ec = secs::ii::encode(rendered, out);
        if (ec) {
            return from_error_code(ec);
        }

        if (!out.empty()) {
            auto *buf = static_cast<uint8_t *>(secs_malloc(out.size()));
            if (!buf)
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            std::memcpy(buf, out.data(), out.size());
            *out_body_bytes = buf;
            *out_body_n = out.size();
        }

        if (out_stream)
            *out_stream = msg->stream;
        if (out_function)
            *out_function = msg->function;
        if (out_w_bit)
            *out_w_bit = msg->w_bit ? 1 : 0;
        return ok();
    });
}

// ----------------------------- HSMS 连接/会话 -----------------------------

void secs_hsms_data_message_free(secs_hsms_data_message_t *msg) {
    guard_void([&]() {
        if (!msg)
            return;
        if (msg->body) {
            secs_free(msg->body);
        }
        msg->body = nullptr;
        msg->body_n = 0;
    });
}

secs_error_t
secs_hsms_connection_create_memory_duplex(secs_context_t *ctx,
                                          secs_hsms_connection_t **out_client,
                                          secs_hsms_connection_t **out_server) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !out_client || !out_server)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_client = nullptr;
        *out_server = nullptr;

        auto c2s = std::make_shared<MemoryChannel>();
        auto s2c = std::make_shared<MemoryChannel>();

        auto client_stream =
            std::make_unique<MemoryStream>(ctx->ioc.get_executor(), s2c, c2s);
        auto server_stream =
            std::make_unique<MemoryStream>(ctx->ioc.get_executor(), c2s, s2c);

        auto *client = new (std::nothrow) secs_hsms_connection(
            secs::hsms::Connection(std::move(client_stream)));
        if (!client)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        auto *server = new (std::nothrow) secs_hsms_connection(
            secs::hsms::Connection(std::move(server_stream)));
        if (!server) {
            delete client;
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        *out_client = client;
        *out_server = server;
        return ok();
    });
}

void secs_hsms_connection_destroy(secs_hsms_connection_t *c) {
    guard_void([&]() { delete c; });
}

secs_error_t
secs_hsms_session_create(secs_context_t *ctx,
                         const secs_hsms_session_options_t *options,
                         secs_hsms_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !options || !out_sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_sess = nullptr;

        auto *h = new (std::nothrow) secs_hsms_session{};
        if (!h)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);

        h->ctx = ctx;
        const auto opt = make_hsms_options(options);

        h->options = opt;
        try {
            h->sess = std::make_shared<secs::hsms::Session>(
                ctx->ioc.get_executor(), opt);
        } catch (...) {
            delete h;
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        *out_sess = h;
        return ok();
    });
}

secs_error_t
secs_hsms_session_create_v2(secs_context_t *ctx,
                            const secs_hsms_session_options_v2_t *options,
                            secs_hsms_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !options || !out_sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_sess = nullptr;

        auto *h = new (std::nothrow) secs_hsms_session{};
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        h->ctx = ctx;
        const auto opt = make_hsms_options_v2(options);

        h->options = opt;
        try {
            h->sess = std::make_shared<secs::hsms::Session>(
                ctx->ioc.get_executor(), opt);
        } catch (...) {
            delete h;
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        *out_sess = h;
        return ok();
    });
}

static secs_error_t hsms_stop_on_io_thread(secs_hsms_session_t *sess) {
    if (!sess || !sess->ctx || !sess->sess)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);

    if (is_io_thread(sess->ctx)) {
        sess->sess->stop();
        return ok();
    }

    // 复制 shared_ptr：避免 stop() 先 post、再立刻 destroy()
    // 导致回调访问已释放对象（UAF）。
    auto s = sess->sess;
    asio::post(sess->ctx->ioc, [s]() { s->stop(); });
    return ok();
}

secs_error_t secs_hsms_session_stop(secs_hsms_session_t *sess) {
    return guard_error(
        [&]() -> secs_error_t { return hsms_stop_on_io_thread(sess); });
}

secs_error_t secs_hsms_session_open_active_ip(secs_hsms_session_t *sess,
                                              const char *ip,
                                              uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        std::error_code parse_ec{};
        auto addr = asio::ip::make_address(ip, parse_ec);
        if (parse_ec)
            return from_error_code(parse_ec);

        asio::ip::tcp::endpoint ep{addr, port};
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, ep]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_open_active(ep);
            });
    });
}

secs_error_t secs_hsms_session_open_passive_ip(secs_hsms_session_t *sess,
                                               const char *ip,
                                               uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        std::error_code parse_ec{};
        auto addr = asio::ip::make_address(ip, parse_ec);
        if (parse_ec)
            return from_error_code(parse_ec);

        asio::ip::tcp::endpoint ep{addr, port};
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, ep]() -> asio::awaitable<std::error_code> {
                asio::ip::tcp::acceptor acceptor{s->executor()};
                std::error_code ec{};

                acceptor.open(ep.protocol(), ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true),
                                    ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.bind(ep, ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.listen(asio::socket_base::max_listen_connections, ec);
                if (ec) {
                    co_return ec;
                }

                auto [acc_ec, socket] = co_await acceptor.async_accept(
                    asio::as_tuple(asio::use_awaitable));
                if (acc_ec) {
                    co_return acc_ec;
                }

                co_return co_await s->async_open_passive(std::move(socket));
            });
    });
}

static secs_error_t hsms_open_with_connection(secs_hsms_session_t *sess,
                                              secs_hsms_connection_t **io_conn,
                                              bool passive) {
    if (!sess || !sess->ctx || !sess->sess)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    if (!io_conn || !*io_conn)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);

    // 取走连接对象（调用后 io_conn 置空，避免误用）
    secs::hsms::Connection conn = std::move((*io_conn)->conn);
    secs_hsms_connection_destroy(*io_conn);
    *io_conn = nullptr;

    if (passive) {
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, conn = std::move(conn)]() mutable
            -> asio::awaitable<std::error_code> {
                co_return co_await s->async_open_passive(std::move(conn));
            });
    }

    return run_blocking_ec(
        sess->ctx,
        [s = sess->sess,
         conn = std::move(conn)]() mutable -> asio::awaitable<std::error_code> {
            co_return co_await s->async_open_active(std::move(conn));
        });
}

secs_error_t
secs_hsms_session_open_active_connection(secs_hsms_session_t *sess,
                                         secs_hsms_connection_t **io_conn) {
    return guard_error([&]() -> secs_error_t {
        return hsms_open_with_connection(sess, io_conn, false);
    });
}

secs_error_t
secs_hsms_session_open_passive_connection(secs_hsms_session_t *sess,
                                          secs_hsms_connection_t **io_conn) {
    return guard_error([&]() -> secs_error_t {
        return hsms_open_with_connection(sess, io_conn, true);
    });
}

secs_error_t secs_hsms_session_is_selected(const secs_hsms_session_t *sess,
                                           int *out_selected) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_selected)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_selected = 0;

        bool selected = false;
        auto bridge = run_blocking<bool>(
            sess->ctx,
            [s = sess->sess]() -> asio::awaitable<bool> {
                co_return s->is_selected();
            },
            selected);
        if (!secs_error_is_ok(bridge))
            return bridge;

        *out_selected = selected ? 1 : 0;
        return ok();
    });
}

void secs_hsms_session_destroy(secs_hsms_session_t *sess) {
    guard_void([&]() {
        if (!sess)
            return;
        if (!sess->ctx || !sess->sess) {
            delete sess;
            return;
        }

        // 若在 io 线程调用 destroy，为避免死锁/悬挂协程，改为“异步销毁”。
        if (is_io_thread(sess->ctx)) {
            asio::co_spawn(
                sess->ctx->ioc,
                [sess]() -> asio::awaitable<void> {
                    sess->sess->stop();
                    (void)co_await sess->sess->async_wait_reader_stopped(
                        std::chrono::seconds(5));
                    delete sess;
                },
                asio::detached);
            return;
        }

        // 同步销毁：先 stop，再等待 reader_loop_ 退出，最后释放对象。
        (void)run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                s->stop();
                co_return std::error_code{};
            });
        (void)run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_wait_reader_stopped(
                    std::chrono::seconds(5));
            });
        delete sess;
    });
}

secs_error_t secs_hsms_session_linktest(secs_hsms_session_t *sess) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_linktest();
            });
    });
}

secs_error_t
secs_hsms_session_send_data_auto_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              const uint8_t *body_bytes,
                                              size_t body_n,
                                              uint32_t *out_system_bytes) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, std::uint32_t>;
        Result result{};
        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess,
             sid = sess->options.session_id,
             stream,
             function,
             w = (w_bit != 0),
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<Result> {
                const auto sb = s->allocate_system_bytes();
                const auto msg = secs::hsms::make_data_message(
                    sid, stream, function, w, sb, body);
                auto ec = co_await s->async_send(msg);
                co_return Result{ec, sb};
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;

        if (out_system_bytes) {
            *out_system_bytes = result.second;
        }
        return from_error_code(result.first);
    });
}

secs_error_t
secs_hsms_session_send_data_with_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              uint32_t system_bytes,
                                              const uint8_t *body_bytes,
                                              size_t body_n) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess,
             sid = sess->options.session_id,
             stream,
             function,
             w = (w_bit != 0),
             system_bytes,
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<std::error_code> {
                const auto msg = secs::hsms::make_data_message(
                    sid, stream, function, w, system_bytes, body);
                co_return co_await s->async_send(msg);
            });
    });
}

secs_error_t secs_hsms_session_receive_data(secs_hsms_session_t *sess,
                                            uint32_t timeout_ms,
                                            secs_hsms_data_message_t *out_msg) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_msg)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::hsms::Message>;
        Result result{};
        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess, timeout = ms_to_optional_duration(timeout_ms)]()
                -> asio::awaitable<Result> {
                co_return co_await s->async_receive_data(timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_hsms_out_message(result.second, out_msg);
    });
}

secs_error_t
secs_hsms_session_request_data(secs_hsms_session_t *sess,
                               uint8_t stream,
                               uint8_t function,
                               const uint8_t *body_bytes,
                               size_t body_n,
                               uint32_t timeout_ms,
                               secs_hsms_data_message_t *out_reply) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_reply)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::hsms::Message>;
        Result result{};

        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess,
             stream,
             function,
             body =
                 bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
             timeout = ms_to_optional_duration(
                 timeout_ms)]() -> asio::awaitable<Result> {
                co_return co_await s->async_request_data(
                    stream, function, body, timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_hsms_out_message(result.second, out_reply);
    });
}

// ----------------------------- 协议会话（protocol::Session）
// -----------------------------

void secs_data_message_free(secs_data_message_t *msg) {
    if (!msg)
        return;
    if (msg->body) {
        secs_free(msg->body);
    }
    msg->body = nullptr;
    msg->body_n = 0;
}

static secs::protocol::SessionOptions
make_proto_options(const secs_protocol_session_options_t *options) {
    secs::protocol::SessionOptions out{};
    if (!options) {
        return out;
    }
    out.t3 = ms_to_duration_or_default(options->t3_ms, out.t3);
    out.poll_interval =
        ms_to_duration_or_default(options->poll_interval_ms, out.poll_interval);
    return out;
}

static void proto_dump_sink_bridge(void *user,
                                   const char *data,
                                   std::size_t size) noexcept {
    auto *state = static_cast<protocol_state *>(user);
    if (!state || !state->dump_sink || !data || size == 0) {
        return;
    }
    try {
        state->dump_sink(state->dump_sink_user, data, size);
    } catch (...) {
        // C callback 不应抛异常；这里吞掉，避免 noexcept 触发 terminate。
    }
}

static secs::protocol::SessionOptions
make_proto_options_v2(const secs_protocol_session_options_v2_t *options,
                      protocol_state *state) {
    secs::protocol::SessionOptions out{};
    if (!options) {
        return out;
    }

    out.t3 = ms_to_duration_or_default(options->t3_ms, out.t3);
    out.poll_interval =
        ms_to_duration_or_default(options->poll_interval_ms, out.poll_interval);

    if (options->max_pending_requests != 0) {
        out.max_pending_requests = options->max_pending_requests;
    }

    const auto flags = options->dump_flags;
    if ((flags & SECS_PROTOCOL_DUMP_ENABLE) != 0) {
        out.dump.enable = true;

        const bool want_tx = (flags & SECS_PROTOCOL_DUMP_TX) != 0;
        const bool want_rx = (flags & SECS_PROTOCOL_DUMP_RX) != 0;
        if (!want_tx && !want_rx) {
            out.dump.dump_tx = true;
            out.dump.dump_rx = true;
        } else {
            out.dump.dump_tx = want_tx;
            out.dump.dump_rx = want_rx;
        }

        if ((flags & SECS_PROTOCOL_DUMP_COLOR) != 0) {
            out.dump.hsms.enable_color = true;
            out.dump.hsms.hex.enable_color = true;
            out.dump.hsms.item.enable_color = true;
            out.dump.secs1.enable_color = true;
            out.dump.secs1.hex.enable_color = true;
            out.dump.secs1.item.enable_color = true;
        }

        if ((flags & SECS_PROTOCOL_DUMP_SECS2_DECODE) != 0) {
            out.dump.hsms.enable_secs2_decode = true;
            out.dump.secs1.enable_secs2_decode = true;
        }

        if (options->dump_sink && state) {
            state->dump_sink = options->dump_sink;
            state->dump_sink_user = options->dump_sink_user;
            out.dump.sink = proto_dump_sink_bridge;
            out.dump.sink_user = state;
        }
    }

    return out;
}

secs_error_t secs_protocol_session_create_from_hsms(
    secs_context_t *ctx,
    secs_hsms_session_t *hsms_sess,
    uint16_t session_id,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !hsms_sess || !hsms_sess->sess || !out_sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (hsms_sess->ctx != ctx) {
            // 协议层与 HSMS 会话必须共享同一个
            // context，否则执行器/线程模型会被破坏。
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_sess = nullptr;

        auto handle = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!handle) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        auto state = std::make_shared<protocol_state>();
        state->ctx = ctx;
        state->hsms_keepalive = hsms_sess->sess;
        state->sess = std::make_unique<secs::protocol::Session>(
            *state->hsms_keepalive, session_id, make_proto_options(options));

        // 启动 async_run：保证请求-响应匹配与入站路由在后台持续运行。
        // 注意：协程捕获 shared_ptr，确保即使 C 侧提前 destroy，run_loop 也不会
        // UAF。
        asio::co_spawn(
            ctx->ioc,
            [state]() -> asio::awaitable<void> {
                co_await state->sess->async_run();
            },
            [state](std::exception_ptr) { state->run_done.set(); });

        handle->state = std::move(state);
        *out_sess = handle.release();
        return ok();
    });
}

secs_error_t secs_protocol_session_create_from_hsms_v2(
    secs_context_t *ctx,
    secs_hsms_session_t *hsms_sess,
    uint16_t session_id,
    const secs_protocol_session_options_v2_t *options,
    secs_protocol_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !hsms_sess || !hsms_sess->sess || !out_sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (hsms_sess->ctx != ctx) {
            // 协议层与 HSMS 会话必须共享同一个
            // context，否则执行器/线程模型会被破坏。
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_sess = nullptr;

        auto handle = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!handle) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        auto state = std::make_shared<protocol_state>();
        state->ctx = ctx;
        state->hsms_keepalive = hsms_sess->sess;
        state->sess = std::make_unique<secs::protocol::Session>(
            *state->hsms_keepalive,
            session_id,
            make_proto_options_v2(options, state.get()));

        // 启动 async_run：保证请求-响应匹配与入站路由在后台持续运行。
        // 注意：协程捕获 shared_ptr，确保即使 C 侧提前 destroy，run_loop 也不会
        // UAF。
        asio::co_spawn(
            ctx->ioc,
            [state]() -> asio::awaitable<void> {
                co_await state->sess->async_run();
            },
            [state](std::exception_ptr) { state->run_done.set(); });

        handle->state = std::move(state);
        *out_sess = handle.release();
        return ok();
    });
}

static secs_error_t proto_stop_on_io_thread(secs_protocol_session_t *sess) {
    if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }

    auto state = sess->state;
    if (is_io_thread(state->ctx)) {
        state->sess->stop();
        return ok();
    }
    asio::post(state->ctx->ioc, [state]() { state->sess->stop(); });
    return ok();
}

secs_error_t secs_protocol_session_stop(secs_protocol_session_t *sess) {
    return guard_error(
        [&]() -> secs_error_t { return proto_stop_on_io_thread(sess); });
}

void secs_protocol_session_destroy(secs_protocol_session_t *sess) {
    guard_void([&]() {
        if (!sess)
            return;

        // 即便 state 为空，也允许释放 handle 本身。
        const auto state = sess->state;
        if (!state || !state->ctx || !state->sess) {
            delete sess;
            return;
        }

        // io 线程内不能阻塞等待；这里仅 stop 并释放 handle，让 run_loop
        // 自行结束并释放 state。
        if (is_io_thread(state->ctx)) {
            state->sess->stop();
            delete sess;
            return;
        }

        (void)proto_stop_on_io_thread(sess);
        (void)run_blocking_ec(
            state->ctx, [state]() -> asio::awaitable<std::error_code> {
                co_return co_await state->run_done.async_wait(std::nullopt);
            });

        delete sess;
    });
}

secs_error_t secs_protocol_session_set_handler(secs_protocol_session_t *sess,
                                               uint8_t stream,
                                               uint8_t function,
                                               secs_protocol_handler_fn cb,
                                               void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        auto make_handler =
            [cb, user_data](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            uint8_t *out_body = nullptr;
            size_t out_n = 0;
            try {
                secs_data_message_view_t view{};
                view.stream = msg.stream;
                view.function = msg.function;
                view.w_bit = msg.w_bit ? 1 : 0;
                view.system_bytes = msg.system_bytes;
                view.body = reinterpret_cast<const uint8_t *>(msg.body.data());
                view.body_n = msg.body.size();

                secs_error_t cec = cb(user_data, &view, &out_body, &out_n);

                if (!secs_error_is_ok(cec)) {
                    if (out_body) {
                        secs_free(out_body);
                    }
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                if (!out_body && out_n != 0) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                std::vector<byte> rsp;
                rsp.resize(out_n);
                if (out_n != 0) {
                    std::memcpy(rsp.data(), out_body, out_n);
                }
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        std::move(rsp)};
            } catch (...) {
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }
        };

        sess->state->sess->router().set(stream, function, std::move(make_handler));

        return ok();
    });
}

secs_error_t
secs_protocol_session_set_default_handler(secs_protocol_session_t *sess,
                                          secs_protocol_handler_fn cb,
                                          void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        auto make_handler =
            [cb, user_data](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            uint8_t *out_body = nullptr;
            size_t out_n = 0;
            try {
                secs_data_message_view_t view{};
                view.stream = msg.stream;
                view.function = msg.function;
                view.w_bit = msg.w_bit ? 1 : 0;
                view.system_bytes = msg.system_bytes;
                view.body = reinterpret_cast<const uint8_t *>(msg.body.data());
                view.body_n = msg.body.size();

                secs_error_t cec = cb(user_data, &view, &out_body, &out_n);

                if (!secs_error_is_ok(cec)) {
                    if (out_body) {
                        secs_free(out_body);
                    }
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                if (!out_body && out_n != 0) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                std::vector<byte> rsp;
                rsp.resize(out_n);
                if (out_n != 0) {
                    std::memcpy(rsp.data(), out_body, out_n);
                }
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        std::move(rsp)};
            } catch (...) {
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }
        };

        sess->state->sess->router().set_default(std::move(make_handler));
        return ok();
    });
}

secs_error_t
secs_protocol_session_clear_default_handler(secs_protocol_session_t *sess) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().clear_default();
        return ok();
    });
}

secs_error_t
secs_protocol_session_set_sml_default_handler(secs_protocol_session_t *sess,
                                              const secs_sml_runtime_t *rt) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !rt) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!rt->rt.loaded()) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // 拷贝 runtime：避免 C 侧销毁 rt 导致 handler UAF。
        auto runtime =
            std::make_shared<secs::sml::Runtime>(rt->rt); // 可能分配/失败

        auto make_handler =
            [runtime](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            try {
                // protocol::Session 的 auto-reply 仅在 W=1 时发送 secondary，因此这里
                // 对 W=0 直接短路，避免不必要的解码开销。
                if (!msg.w_bit) {
                    co_return secs::protocol::HandlerResult{std::error_code{}, {}};
                }
                if (msg.function == 0xFFu) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                secs::ii::Item decoded{secs::ii::List{}};
                std::size_t consumed = 0;
                const auto dec_ec = secs::ii::decode_one(
                    bytes_view{msg.body.data(), msg.body.size()}, decoded, consumed);
                if (dec_ec) {
                    co_return secs::protocol::HandlerResult{dec_ec, {}};
                }

                auto matched = runtime->match_response(msg.stream, msg.function, decoded);
                if (!matched.has_value()) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                const auto *rsp = runtime->get_message(*matched);
                if (!rsp) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                const auto expected_function =
                    static_cast<std::uint8_t>(msg.function + 1u);
                if (rsp->stream != msg.stream || rsp->function != expected_function ||
                    rsp->w_bit) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                // 当前 C API 的 SML default handler 不支持变量注入：用空上下文渲染。
                secs::sml::RenderContext ctx{};
                secs::ii::Item rendered{secs::ii::List{}};
                const auto render_ec =
                    secs::sml::render_item(rsp->item, ctx, rendered);
                if (render_ec) {
                    co_return secs::protocol::HandlerResult{render_ec, {}};
                }

                std::vector<byte> out;
                const auto enc_ec = secs::ii::encode(rendered, out);
                if (enc_ec) {
                    co_return secs::protocol::HandlerResult{enc_ec, {}};
                }

                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        std::move(out)};
            } catch (const std::bad_alloc &) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::out_of_memory), {}};
            } catch (...) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }
        };

        sess->state->sess->router().set_default(std::move(make_handler));
        return ok();
    });
}

secs_error_t secs_protocol_session_erase_handler(secs_protocol_session_t *sess,
                                                 uint8_t stream,
                                                 uint8_t function) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().erase(stream, function);
        return ok();
    });
}

secs_error_t secs_protocol_session_send(secs_protocol_session_t *sess,
                                        uint8_t stream,
                                        uint8_t function,
                                        const uint8_t *body_bytes,
                                        size_t body_n) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        auto state = sess->state;
        return run_blocking_ec(
            state->ctx,
            [state,
             stream,
             function,
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<std::error_code> {
                co_return co_await state->sess->async_send(
                    stream, function, body);
            });
    });
}

secs_error_t secs_protocol_session_request(secs_protocol_session_t *sess,
                                           uint8_t stream,
                                           uint8_t function,
                                           const uint8_t *body_bytes,
                                           size_t body_n,
                                           uint32_t timeout_ms,
                                           secs_data_message_t *out_reply) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess ||
            !out_reply)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::protocol::DataMessage>;
        Result result{};

        auto state = sess->state;
        auto bridge = run_blocking<Result>(
            state->ctx,
            [state,
             stream,
             function,
             body =
                 bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
             timeout = ms_to_optional_duration(
                 timeout_ms)]() -> asio::awaitable<Result> {
                co_return co_await state->sess->async_request(
                    stream, function, body, timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_protocol_out_message(result.second, out_reply);
    });
}
