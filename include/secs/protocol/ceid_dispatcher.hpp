#pragma once

#include "secs/core/error.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/router.hpp"
#include "secs/utils/ii_helpers.hpp"

#include <asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace secs::protocol {

/**
 * @brief 用于“消息体携带 CEID”的简易处理层。
 *
 * 设计目标：
 * - 不引入 GEM(E30) 语义，仅提供：解码 body -> 提取 CEID -> 按 CEID 分发 handler；
 * - 适用于厂商文档定义的“CEID 触发事件/请求-响应”类型消息（例如 S6F11/S6F12 等）。
 *
 * 使用方式（示意）：
 * 1) 定义 CEID 提取器（从已解码的 Item 中拿到 CEID）
 * 2) 为不同 CEID 注册处理器
 * 3) 将 dispatcher 的 invoke() 挂到 protocol::Router 的某个 SxFy 上
 */
class CeidDispatcher final {
public:
    using Ceid = std::uint32_t;

    struct DecodeOptions final {
        // SECS-II 解码资源限制（用于约束不可信输入的资源消耗）。
        secs::ii::DecodeLimits limits{};

        // 是否要求 consumed==msg.body.size()（严格消费整个输入）。
        // - true：若存在尾随 bytes，返回 invalid_argument；
        // - false：允许尾随 bytes。
        bool strict_consumed{true};
    };

    // 从“已解码 Item”中提取 CEID。
    using Extractor =
        std::function<std::optional<Ceid>(const DataMessage &, const secs::ii::Item &)>;

    // CEID handler：返回可直接用于回包的 body bytes（由 session 自动回 secondary）。
    using Handler =
        std::function<asio::awaitable<HandlerResult>(Ceid,
                                                     const secs::ii::Item &,
                                                     const DataMessage &)>;

    // Item handler：返回一个 Item，由 CeidDispatcher 负责 encode 为 bytes。
    using ItemHandlerResult = std::pair<std::error_code, secs::ii::Item>;
    using ItemHandler =
        std::function<asio::awaitable<ItemHandlerResult>(Ceid,
                                                         const secs::ii::Item &,
                                                         const DataMessage &)>;

    explicit CeidDispatcher(Extractor extractor);
    CeidDispatcher(Extractor extractor, DecodeOptions options);

    void set(Ceid ceid, Handler handler);
    void set_item(Ceid ceid, ItemHandler handler);
    void set_default(Handler handler);
    void set_default_item(ItemHandler handler);
    void erase(Ceid ceid) noexcept;
    void clear_default() noexcept;
    void clear() noexcept;

    /**
     * @brief protocol::Router 侧调用入口：解码 + 提取 CEID + 分发。
     *
     * 错误处理：
     * - body 为空：invalid_argument
     * - SECS-II 解码失败：传播 ii::errc
     * - strict_consumed 且未完整消费：invalid_argument
     * - 提取 CEID 失败：invalid_argument
     * - 未注册 CEID 且无默认处理器：invalid_argument
     */
    asio::awaitable<HandlerResult> invoke(const DataMessage &msg);

    [[nodiscard]] static Handler make_item_handler(ItemHandler handler);

private:
    [[nodiscard]] std::optional<Handler> find_(Ceid ceid) const;

    Extractor extractor_{};
    DecodeOptions decode_options_{};

    mutable std::mutex mu_{};
    std::unordered_map<Ceid, Handler> handlers_{};
    std::optional<Handler> default_handler_{};
};

/**
 * @brief 将 CeidDispatcher 注册到 Router（shared_ptr 保证生命周期）。
 */
template <typename TDispatcher>
inline void register_ceid_dispatcher(Router &router,
                                     std::uint8_t stream,
                                     std::uint8_t function,
                                     std::shared_ptr<TDispatcher> dispatcher) {
    router.set(stream, function, [dispatcher](const DataMessage &msg)
                                    -> asio::awaitable<HandlerResult> {
        co_return co_await dispatcher->invoke(msg);
    });
}

inline CeidDispatcher::CeidDispatcher(Extractor extractor)
    : CeidDispatcher(std::move(extractor), DecodeOptions{}) {}

inline CeidDispatcher::CeidDispatcher(Extractor extractor, DecodeOptions options)
    : extractor_(std::move(extractor)), decode_options_(std::move(options)) {}

inline void CeidDispatcher::set(Ceid ceid, Handler handler) {
    std::lock_guard lk(mu_);
    handlers_[ceid] = std::move(handler);
}

inline void CeidDispatcher::set_item(Ceid ceid, ItemHandler handler) {
    set(ceid, make_item_handler(std::move(handler)));
}

inline void CeidDispatcher::set_default(Handler handler) {
    std::lock_guard lk(mu_);
    default_handler_ = std::move(handler);
}

inline void CeidDispatcher::set_default_item(ItemHandler handler) {
    set_default(make_item_handler(std::move(handler)));
}

inline void CeidDispatcher::erase(Ceid ceid) noexcept {
    std::lock_guard lk(mu_);
    handlers_.erase(ceid);
}

inline void CeidDispatcher::clear_default() noexcept {
    std::lock_guard lk(mu_);
    default_handler_.reset();
}

inline void CeidDispatcher::clear() noexcept {
    std::lock_guard lk(mu_);
    handlers_.clear();
    default_handler_.reset();
}

inline std::optional<CeidDispatcher::Handler> CeidDispatcher::find_(
    Ceid ceid) const {
    std::lock_guard lk(mu_);
    const auto it = handlers_.find(ceid);
    if (it != handlers_.end()) {
        return it->second;
    }
    if (default_handler_.has_value()) {
        return *default_handler_;
    }
    return std::nullopt;
}

inline asio::awaitable<HandlerResult> CeidDispatcher::invoke(
    const DataMessage &msg) {
    if (msg.body.empty()) {
        co_return HandlerResult{
            secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
    }

    const secs::core::bytes_view body_view{msg.body.data(), msg.body.size()};
    auto [dec_ec, decoded] =
        secs::utils::decode_one_item(body_view, decode_options_.limits);
    if (dec_ec) {
        co_return HandlerResult{dec_ec, {}};
    }
    if (decode_options_.strict_consumed && !decoded.fully_consumed) {
        co_return HandlerResult{
            secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
    }

    if (!extractor_) {
        co_return HandlerResult{
            secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
    }

    const auto ceid_opt = extractor_(msg, decoded.item);
    if (!ceid_opt.has_value()) {
        co_return HandlerResult{
            secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
    }

    auto handler_opt = find_(*ceid_opt);
    if (!handler_opt.has_value()) {
        co_return HandlerResult{
            secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
    }

    // 重要：不要在持锁状态下 co_await，避免死锁与阻塞 set()/erase()。
    auto handler = std::move(*handler_opt);
    co_return co_await handler(*ceid_opt, decoded.item, msg);
}

inline CeidDispatcher::Handler
CeidDispatcher::make_item_handler(ItemHandler handler) {
    return [handler = std::move(handler)](
               Ceid ceid,
               const secs::ii::Item &request_body,
               const DataMessage &raw) -> asio::awaitable<HandlerResult> {
        auto [ec, response_item] = co_await handler(ceid, request_body, raw);
        if (ec) {
            co_return HandlerResult{ec, {}};
        }

        auto [enc_ec, out] = secs::utils::encode_item(response_item);
        co_return HandlerResult{enc_ec, std::move(out)};
    };
}

} // namespace secs::protocol
