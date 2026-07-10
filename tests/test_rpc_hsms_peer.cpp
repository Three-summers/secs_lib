#include "secs/core/error.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

struct Options final {
    std::string listen_address{"127.0.0.1:50061"};
    std::uint16_t session_id{1};
    std::uint32_t reply_delay_ms{0};
    bool drop_s1f1{false};
};

bool parse_u16(std::string_view text, std::uint16_t &out) {
    unsigned value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value > 0xFFFFu) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_u32(std::string_view text, std::uint32_t &out) {
    std::uint32_t value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = value;
    return true;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options options{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--listen") {
            const char *value = need_value("--listen");
            if (!value) {
                return std::nullopt;
            }
            options.listen_address = value;
            continue;
        }
        if (arg == "--session-id") {
            const char *value = need_value("--session-id");
            if (!value || !parse_u16(value, options.session_id)) {
                std::cerr << "invalid value for --session-id\n";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--reply-delay-ms") {
            const char *value = need_value("--reply-delay-ms");
            if (!value || !parse_u32(value, options.reply_delay_ms)) {
                std::cerr << "invalid value for --reply-delay-ms\n";
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--drop-s1f1") {
            options.drop_s1f1 = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            std::cout
                << "usage: test_rpc_hsms_peer --listen <ip:port> "
                   "[--session-id <u16>] [--reply-delay-ms <u32>] "
                   "[--drop-s1f1]\n";
            return std::nullopt;
        }
        std::cerr << "unknown option: " << arg << "\n";
        return std::nullopt;
    }

    return options;
}

std::optional<asio::ip::tcp::endpoint>
parse_endpoint(std::string_view listen_address) {
    const auto pos = listen_address.rfind(':');
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto host = listen_address.substr(0, pos);
    const auto port_text = listen_address.substr(pos + 1);
    std::uint16_t port = 0;
    if (!parse_u16(port_text, port) || port == 0) {
        return std::nullopt;
    }

    std::error_code ec{};
    const auto address = asio::ip::make_address(std::string(host), ec);
    if (ec) {
        return std::nullopt;
    }
    return asio::ip::tcp::endpoint(address, port);
}

asio::awaitable<secs::protocol::HandlerResult>
handle_s1f1(const secs::protocol::DataMessage &message,
            std::uint32_t reply_delay_ms,
            bool drop_s1f1) {
    if (reply_delay_ms > 0) {
        const auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds{reply_delay_ms});
        auto [delay_ec] =
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (delay_ec) {
            co_return secs::protocol::HandlerResult{delay_ec, {}};
        }
    }
    if (drop_s1f1) {
        co_return secs::protocol::HandlerResult{
            std::make_error_code(std::errc::operation_canceled), {}};
    }

    secs::ii::Item request_item = secs::ii::Item::list({});
    if (!message.body.empty()) {
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            bytes_view{message.body.data(), message.body.size()},
            request_item,
            consumed);
        if (ec || consumed != message.body.size()) {
            co_return secs::protocol::HandlerResult{
                ec ? ec : make_error_code(errc::invalid_argument), {}};
        }
    }

    secs::ii::Item reply = secs::ii::Item::list({
        secs::ii::Item::ascii("ACK"),
        std::move(request_item),
    });
    std::vector<byte> encoded;
    const auto encode_ec = secs::ii::encode(reply, encoded);
    co_return secs::protocol::HandlerResult{encode_ec, std::move(encoded)};
}

asio::awaitable<int> run_peer(const Options &options) {
    const auto ex = co_await asio::this_coro::executor;
    const auto endpoint = parse_endpoint(options.listen_address);
    if (!endpoint.has_value()) {
        std::cerr << "invalid --listen address\n";
        co_return 2;
    }

    asio::ip::tcp::acceptor acceptor(ex, *endpoint);
    std::cout << "secs-rpc-hsms-peer listening on " << options.listen_address
              << "\n";
    std::cout.flush();

    auto [accept_ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (accept_ec) {
        std::cerr << "accept failed: " << accept_ec.message() << "\n";
        co_return 1;
    }

    secs::hsms::SessionOptions hsms_options{};
    hsms_options.session_id = options.session_id;
    hsms_options.auto_reconnect = false;
    hsms_options.t3 = std::chrono::seconds{3};
    hsms_options.t5 = std::chrono::milliseconds{200};
    hsms_options.t6 = std::chrono::seconds{3};
    hsms_options.t7 = std::chrono::seconds{3};
    hsms_options.t8 = std::chrono::seconds{3};

    secs::hsms::Session hsms(ex, hsms_options);
    const auto open_ec = co_await hsms.async_open_passive(std::move(socket));
    if (open_ec) {
        std::cerr << "open_passive failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    secs::protocol::SessionOptions protocol_options{};
    protocol_options.t3 = std::chrono::seconds{3};
    protocol_options.poll_interval = std::chrono::milliseconds{10};

    secs::protocol::Session protocol(hsms, options.session_id, protocol_options);
    protocol.router().set(
        1,
        1,
        [reply_delay_ms = options.reply_delay_ms,
         drop_s1f1 = options.drop_s1f1](
            const secs::protocol::DataMessage &message)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            co_return co_await handle_s1f1(
                message, reply_delay_ms, drop_s1f1);
        });

    std::cout << "secs-rpc-hsms-peer selected\n";
    std::cout.flush();

    co_await protocol.async_run();
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    const auto options = parse_args(argc, argv);
    if (!options.has_value()) {
        return 2;
    }

    asio::io_context io_context;
    int rc = 1;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            rc = co_await run_peer(*options);
            io_context.stop();
            co_return;
        },
        asio::detached);
    io_context.run();
    return rc;
}
