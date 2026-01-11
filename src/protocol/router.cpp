#include "secs/protocol/router.hpp"

namespace secs::protocol {

/*
 * 协议层 Router 实现（Stream/Function -> Handler）。
 *
 * 设计取舍：
 * - 采用固定 key=(stream<<8|function) 的哈希表，并提供一个 stream-only fallback
 *   （SxF*）以减少样板代码；
 * - 通过互斥锁保护 handlers_，允许在多线程/多协程环境下动态注册/查询；
 * - find() 返回 Handler 的拷贝，避免调用方持锁执行 handler 协程导致的死锁/长阻塞。
 */

// Router 是一个简单的 (Stream,Function)->Handler 映射：
// - set/erase/clear/find
// 内部用互斥锁保护，便于在测试或多协程场景下安全注册/查询。
// - find 返回 Handler 的拷贝（std::function
// 可复制），避免调用方持有锁期间执行协程。
void Router::set(std::uint8_t stream, std::uint8_t function, Handler handler) {
    std::lock_guard lk(mu_);
    handlers_.insert_or_assign(make_key_(stream, function), std::move(handler));
}

void Router::set_stream_default(std::uint8_t stream, Handler handler) {
    std::lock_guard lk(mu_);
    stream_default_handlers_.insert_or_assign(stream, std::move(handler));
}

void Router::set_default(Handler handler) {
    std::lock_guard lk(mu_);
    default_handler_ = std::move(handler);
}

void Router::erase(std::uint8_t stream, std::uint8_t function) noexcept {
    std::lock_guard lk(mu_);
    handlers_.erase(make_key_(stream, function));
}

void Router::clear_stream_default(std::uint8_t stream) noexcept {
    std::lock_guard lk(mu_);
    stream_default_handlers_.erase(stream);
}

void Router::clear_default() noexcept {
    std::lock_guard lk(mu_);
    default_handler_.reset();
}

void Router::clear() noexcept {
    std::lock_guard lk(mu_);
    handlers_.clear();
    stream_default_handlers_.clear();
    default_handler_.reset();
}

std::optional<Handler> Router::find(std::uint8_t stream,
                                    std::uint8_t function) const {
    std::lock_guard lk(mu_);
    const auto it = handlers_.find(make_key_(stream, function));
    if (it != handlers_.end()) {
        return it->second;
    }
    const auto sit = stream_default_handlers_.find(stream);
    if (sit != stream_default_handlers_.end()) {
        return sit->second;
    }
    if (default_handler_.has_value()) {
        return *default_handler_;
    }
    return std::nullopt;
}

} // namespace secs::protocol
