#pragma once

#include "secs/core/common.hpp"
#include "secs/secs1/block.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace secs::secs1 {

/**
 * @brief SECS-I 串口链路抽象（字节流，半双工由协议层保证）。
 *
 * 注意：
 * - 该接口不规定底层是串口/内存队列/其他；只要求按字节读写。
 * - 默认假设在同一执行器/线程语境下使用；跨线程需由调用者自行保证顺序。
 */
class Link {
 public:
  virtual ~Link() = default;

  [[nodiscard]] virtual asio::any_io_executor executor() const noexcept = 0;

  virtual asio::awaitable<std::error_code> async_write(secs::core::bytes_view data) = 0;

  virtual asio::awaitable<std::pair<std::error_code, secs::core::byte>> async_read_byte(
    std::optional<secs::core::duration> timeout = std::nullopt) = 0;
};

/**
 * @brief 内存模拟链路（用于单元测试）。
 *
 * 特性：
 * - create() 生成一对 Endpoint，互相连通
 * - 支持简单的发送侧注入：固定延迟/丢弃前 N 个字节（用于模拟 T1/T2/NAK 重传等场景）
 */
class MemoryLink final {
 public:
  class Endpoint final : public Link {
   public:
    Endpoint() = default;

    [[nodiscard]] asio::any_io_executor executor() const noexcept override;

    asio::awaitable<std::error_code> async_write(secs::core::bytes_view data) override;

    asio::awaitable<std::pair<std::error_code, secs::core::byte>> async_read_byte(
      std::optional<secs::core::duration> timeout = std::nullopt) override;

    void drop_next(std::size_t n) noexcept;
    void set_fixed_delay(std::optional<secs::core::duration> delay) noexcept;

   private:
    friend class MemoryLink;

    struct SharedState;

    Endpoint(asio::any_io_executor ex, std::shared_ptr<SharedState> shared, bool is_a);

    asio::any_io_executor executor_{};
    std::shared_ptr<SharedState> shared_{};
    bool is_a_{true};
  };

  static std::pair<Endpoint, Endpoint> create(asio::any_io_executor ex);
};

}  // 命名空间 secs::secs1
