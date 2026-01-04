#include "secs/protocol/router.hpp"

namespace secs::protocol {

/*
 * 协议层 Router 实现（Stream/Function -> Handler）。
 *
 * 设计取舍：
 * - 采用固定 key=(stream<<8|function) 的哈希表，不提供通配符，保持简单可控；
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

void Router::erase(std::uint8_t stream, std::uint8_t function) noexcept {
    std::lock_guard lk(mu_);
    handlers_.erase(make_key_(stream, function));
}

void Router::clear() noexcept {
    std::lock_guard lk(mu_);
    handlers_.clear();
}

std::optional<Handler> Router::find(std::uint8_t stream,
                                    std::uint8_t function) const {
    std::lock_guard lk(mu_);
    const auto it = handlers_.find(make_key_(stream, function));
    if (it == handlers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace secs::protocol
