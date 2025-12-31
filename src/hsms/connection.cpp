#include "secs/hsms/connection.hpp"

#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/cancellation_condition.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <array>

namespace secs::hsms {
namespace {

class TcpStream final : public Stream {
 public:
  explicit TcpStream(asio::any_io_executor ex) : executor_(ex), socket_(ex) {}

  explicit TcpStream(asio::ip::tcp::socket socket)
    : executor_(socket.get_executor()),
      socket_(std::move(socket)) {}

  [[nodiscard]] asio::any_io_executor executor() const noexcept override { return executor_; }
  [[nodiscard]] bool is_open() const noexcept override { return socket_.is_open(); }

  void cancel() noexcept override {
    std::error_code ignored;
    socket_.cancel(ignored);
  }

  void close() noexcept override {
    std::error_code ignored;
    socket_.close(ignored);
  }

  asio::awaitable<std::pair<std::error_code, std::size_t>> async_read_some(
    core::mutable_bytes_view dst) override {
    auto [ec, n] = co_await socket_.async_read_some(
      asio::buffer(dst.data(), dst.size()),
      asio::as_tuple(asio::use_awaitable));
    co_return std::pair{ec, n};
  }

  asio::awaitable<std::error_code> async_write_all(core::bytes_view src) override {
    auto [ec, n] = co_await asio::async_write(
      socket_,
      asio::buffer(src.data(), src.size()),
      asio::as_tuple(asio::use_awaitable));
    if (ec) {
      co_return ec;
    }
    if (n != src.size()) {
      co_return core::make_error_code(core::errc::invalid_argument);
    }
    co_return std::error_code{};
  }

  asio::awaitable<std::error_code> async_connect(const asio::ip::tcp::endpoint& endpoint) override {
    auto [ec] = co_await socket_.async_connect(endpoint, asio::as_tuple(asio::use_awaitable));
    co_return ec;
  }

 private:
  asio::any_io_executor executor_;
  asio::ip::tcp::socket socket_;
};

}  // namespace

Connection::Connection(asio::any_io_executor ex, ConnectionOptions options)
  : stream_(std::make_unique<TcpStream>(ex)), options_(options) {
  write_gate_.set();
}

Connection::Connection(asio::ip::tcp::socket socket, ConnectionOptions options)
  : stream_(std::make_unique<TcpStream>(std::move(socket))), options_(options) {
  write_gate_.set();
}

Connection::Connection(std::unique_ptr<Stream> stream, ConnectionOptions options)
  : stream_(std::move(stream)), options_(options) {
  write_gate_.set();
}

asio::any_io_executor Connection::executor() const noexcept {
  if (!stream_) {
    return asio::any_io_executor{};
  }
  return stream_->executor();
}

bool Connection::is_open() const noexcept {
  return stream_ && stream_->is_open();
}

std::uint32_t Connection::read_u32_be_(const core::byte* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24U) |
         (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) |
         static_cast<std::uint32_t>(p[3]);
}

asio::awaitable<std::error_code> Connection::async_connect(const asio::ip::tcp::endpoint& endpoint) {
  if (!stream_) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }
  co_return co_await stream_->async_connect(endpoint);
}

asio::awaitable<std::error_code> Connection::async_close() {
  if (stream_) {
    stream_->cancel();
    stream_->close();
  }

  // 取消可能正在等待写锁的协程。
  write_gate_.cancel();
  co_return std::error_code{};
}

void Connection::cancel_and_close() noexcept {
  if (stream_) {
    stream_->cancel();
    stream_->close();
  }
  write_gate_.cancel();
}

asio::awaitable<std::pair<std::error_code, std::size_t>> Connection::async_read_some_with_t8(
  core::byte* dst,
  std::size_t n) {
  if (!stream_) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), std::size_t{0}};
  }

  if (options_.t8 == core::duration{}) {
    co_return co_await stream_->async_read_some(core::mutable_bytes_view{dst, n});
  }

  auto ex = co_await asio::this_coro::executor;
  asio::steady_timer timer(ex);
  timer.expires_after(options_.t8);

  auto read_task = asio::co_spawn(
    ex,
    stream_->async_read_some(core::mutable_bytes_view{dst, n}),
    asio::deferred);

  auto timer_task = asio::co_spawn(
    ex,
    timer.async_wait(asio::as_tuple(asio::use_awaitable)),
    asio::deferred);

  auto [order, read_ex, read_result, timer_ex, timer_result] =
    co_await asio::experimental::make_parallel_group(
      std::move(read_task),
      std::move(timer_task))
      .async_wait(asio::experimental::wait_for_one(), asio::as_tuple(asio::use_awaitable));

  (void)timer_result;

  if (read_ex || timer_ex) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), std::size_t{0}};
  }

  if (order[0] == 0) {
    co_return read_result;
  }

  // timer 先完成，按 T8 超时处理。
  stream_->cancel();
  co_return std::pair{core::make_error_code(core::errc::timeout), std::size_t{0}};
}

asio::awaitable<std::error_code> Connection::async_read_exactly(core::mutable_bytes_view dst) {
  std::size_t offset = 0;
  while (offset < dst.size()) {
    auto [ec, n] = co_await async_read_some_with_t8(dst.data() + offset, dst.size() - offset);
    if (ec) {
      co_return ec;
    }
    if (n == 0) {
      co_return core::make_error_code(core::errc::invalid_argument);
    }
    offset += n;
  }
  co_return std::error_code{};
}

asio::awaitable<std::error_code> Connection::async_write_message(const Message& msg) {
  if (!stream_) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }

  while (write_in_progress_) {
    auto ec = co_await write_gate_.async_wait();
    if (ec) {
      co_return ec;
    }
  }
  write_in_progress_ = true;
  write_gate_.reset();

  const auto frame = encode_frame(msg);
  const auto ec = co_await stream_->async_write_all(core::bytes_view{frame.data(), frame.size()});

  write_in_progress_ = false;
  write_gate_.set();
  co_return ec;
}

asio::awaitable<std::pair<std::error_code, Message>> Connection::async_read_message() {
  std::array<core::byte, kLengthFieldSize> len_buf{};
  auto ec = co_await async_read_exactly(core::mutable_bytes_view{len_buf.data(), len_buf.size()});
  if (ec) {
    co_return std::pair{ec, Message{}};
  }

  const std::uint32_t payload_len = read_u32_be_(len_buf.data());
  if (payload_len < kHeaderSize) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), Message{}};
  }
  if (payload_len > kMaxPayloadSize) {
    co_return std::pair{core::make_error_code(core::errc::buffer_overflow), Message{}};
  }

  std::vector<core::byte> payload;
  payload.resize(payload_len);
  ec = co_await async_read_exactly(core::mutable_bytes_view{payload.data(), payload.size()});
  if (ec) {
    co_return std::pair{ec, Message{}};
  }

  Message msg;
  ec = decode_payload(core::bytes_view{payload.data(), payload.size()}, msg);
  if (ec) {
    co_return std::pair{ec, Message{}};
  }
  co_return std::pair{std::error_code{}, std::move(msg)};
}

}  // namespace secs::hsms
