#include "secs/protocol/router.hpp"

namespace secs::protocol {

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

std::optional<Handler> Router::find(std::uint8_t stream, std::uint8_t function) const {
  std::lock_guard lk(mu_);
  const auto it = handlers_.find(make_key_(stream, function));
  if (it == handlers_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace secs::protocol

