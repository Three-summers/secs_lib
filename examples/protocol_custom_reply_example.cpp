/**
 * @file protocol_custom_reply_example.cpp
 * @brief 演示：在 protocol 层用 Router/default handler 实现“按 SxFy 自定义回包”
 *
 * 本示例的关注点是“用户层如何写回包逻辑”，而不是具体传输（HSMS/SECS-I）。
 * 因此这里用 SECS-I 的 MemoryLink 在进程内模拟一根串口线：
 *
 * - Equipment 侧注册一个 default handler（类似 c_dump/TVOC_Secs_App/tvoc_secs_app.c
 *   的 switch(Stream/Function) 风格），按收到的 SxFy 生成对应的回应 body；
 * - handler 只返回“回应 body（SECS-II on-wire bytes）”，protocol::Session 会自动：
 *     * secondary function = primary function + 1
 *     * W=0
 *     * system_bytes 回显为请求的 system_bytes
 *
 * 另外，为了验证“双方都可以主动发送 primary”，示例分成两段：
 * - demo1：Host 主动发起 S1F1/S1F3，Equipment 侧自动回 S1F2/S1F4
 * - demo2：Equipment 主动发起 S2F1，Host 侧自动回 S2F2
 *
 * 注意（SECS-I 半双工约束）：
 * - 同一端不要同时跑 async_run() 又并发调用 async_request()/async_send()，
 *   否则会触发底层 StateMachine 的并发保护（invalid_argument）。
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/core/log.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

using secs::ii::Item;
using secs::ii::List;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using secs::protocol::Session;
using secs::protocol::SessionOptions;

using secs::secs1::MemoryLink;
using secs::secs1::StateMachine;
using secs::secs1::Timeouts;

struct DeviceState final {
    std::string project_id{"TVOC"};
    std::string version_id{"1.0.0"};

    // 用于模拟“设备参数”：10 个通道，分两组返回（类似 tvoc_secs_app.c 的 SFType_NO=0/1）。
    std::array<float, 10> valve_voc_max{
        1.11f, 2.22f, 3.33f, 4.44f, 5.55f,
        6.66f, 7.77f, 8.88f, 9.99f, 10.10f,
    };
};

static std::pair<std::error_code, std::vector<byte>>
encode_item(const Item &item) {
    std::vector<byte> out;
    const auto ec = secs::ii::encode(item, out);
    return {ec, std::move(out)};
}

// 解析请求 body：期望是 U1，取第一个元素作为 “group id”。
static std::pair<std::error_code, std::uint8_t>
decode_u1_first(bytes_view body) {
    if (body.empty()) {
        return {std::error_code{}, 0};
    }

    Item decoded(Item::list({}));
    std::size_t consumed = 0;
    const auto ec = secs::ii::decode_one(body, decoded, consumed);
    if (ec) {
        return {ec, 0};
    }

    const auto *u1 = decoded.get_if<secs::ii::U1>();
    if (!u1 || u1->values.empty()) {
        return {make_error_code(errc::invalid_argument), 0};
    }
    return {std::error_code{}, u1->values[0]};
}

static std::error_code decode_and_print_item(const char *title,
                                             const DataMessage &msg) {
    std::cout << title << " S" << static_cast<int>(msg.stream) << "F"
              << static_cast<int>(msg.function) << " (body=" << msg.body.size()
              << " bytes)\n";

    if (msg.body.empty()) {
        std::cout << "  <empty>\n";
        return {};
    }

    Item decoded(Item::list({}));
    std::size_t consumed = 0;
    const auto ec = secs::ii::decode_one(
        bytes_view{msg.body.data(), msg.body.size()}, decoded, consumed);
    if (ec) {
        std::cout << "  decode_one failed: " << ec.message() << "\n";
        return ec;
    }

    if (const auto *list = decoded.get_if<List>(); list && list->size() == 2) {
        if (const auto *a0 = (*list)[0].get_if<secs::ii::ASCII>()) {
            std::cout << "  [0] ASCII: " << a0->value << "\n";
        }
        if (const auto *a1 = (*list)[1].get_if<secs::ii::ASCII>()) {
            std::cout << "  [1] ASCII: " << a1->value << "\n";
        }
        if (const auto *f4 = (*list)[1].get_if<secs::ii::F4>()) {
            std::cout << "  [1] F4 count=" << f4->values.size() << " values:";
            for (float v : f4->values) {
                std::cout << " " << v;
            }
            std::cout << "\n";
        }
    } else if (const auto *ascii = decoded.get_if<secs::ii::ASCII>()) {
        std::cout << "  ASCII: " << ascii->value << "\n";
    } else if (const auto *f4 = decoded.get_if<secs::ii::F4>()) {
        std::cout << "  F4 count=" << f4->values.size() << " values:";
        for (float v : f4->values) {
            std::cout << " " << v;
        }
        std::cout << "\n";
    } else {
        std::cout << "  (示例：未对该 Item 类型做格式化输出)\n";
    }

    return {};
}

// demo1：Host 主动发起 request，Equipment 侧回包。
static asio::awaitable<void> demo_host_requests_equipment() {
    auto ex = co_await asio::this_coro::executor;

    std::cout << "\n=== demo1: Host -> Equipment (S1F1/S1F3) ===\n";

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 200ms;
    timeouts.t3_reply = 1s;
    timeouts.t4_interblock = 200ms;

    constexpr std::uint16_t device_id = 1;
    auto [host_link, equip_link] = MemoryLink::create(ex);

    StateMachine host_sm(host_link, device_id, timeouts);
    StateMachine equip_sm(equip_link, device_id, timeouts);

    SessionOptions host_opt{};
    host_opt.t3 = 1s;
    host_opt.poll_interval = 10ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment

    SessionOptions equip_opt = host_opt;
    equip_opt.secs1_reverse_bit = true; // Equipment -> Host

    Session host(host_sm, device_id, host_opt);
    Session equip(equip_sm, device_id, equip_opt);

    DeviceState state{};

    // Equipment 侧：集中处理多个 SxFy（类似 tvoc_secs_app.c 的 switch）。
    equip.router().set_default(
        [&state](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            // 这里只演示两条标准消息：
            // - S1F1 -> S1F2：返回设备标识（project/version）
            // - S1F3 -> S1F4：按请求中的 group id 返回一组数值
            if (req.stream != 1) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            switch (req.function) {
            case 1: {
                const Item rsp_item = Item::list({
                    Item::ascii("project: " + state.project_id),
                    Item::ascii("version: " + state.version_id),
                });

                auto [ec, body] = encode_item(rsp_item);
                co_return HandlerResult{ec, std::move(body)};
            }
            case 3: {
                auto [dec_ec, group] = decode_u1_first(
                    bytes_view{req.body.data(), req.body.size()});
                if (dec_ec) {
                    co_return HandlerResult{dec_ec, {}};
                }

                const std::size_t group_index = (group == 0) ? 0U : 1U;
                const std::size_t offset = group_index * 5U;

                std::vector<float> values;
                values.reserve(5);
                for (std::size_t i = 0; i < 5; ++i) {
                    values.push_back(state.valve_voc_max[offset + i]);
                }

                const Item rsp_item = Item::list({
                    Item::ascii("01H1"),
                    Item::f4(std::move(values)),
                });

                auto [ec, body] = encode_item(rsp_item);
                co_return HandlerResult{ec, std::move(body)};
            }
            default:
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }
        });

    // Equipment 侧接收循环（负责接收并自动回包）。
    secs::core::Event equip_done{};
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await equip.async_run(); },
        [&](std::exception_ptr) { equip_done.set(); });

    // Host -> Equipment：S1F1(W=1) 请求（body 为空）
    {
        auto [ec, rsp] =
            co_await host.async_request(1, 1, secs::core::bytes_view{}, 1s);
        if (ec) {
            std::cout << "Host S1F1 request failed: " << ec.message() << "\n";
        } else {
            (void)decode_and_print_item("[Host] recv", rsp);
        }
    }

    // Host -> Equipment：S1F3(W=1) 请求（body=U1{group}）
    for (std::uint8_t group : {static_cast<std::uint8_t>(0),
                               static_cast<std::uint8_t>(1)}) {
        std::vector<byte> req_body;
        (void)secs::ii::encode(Item::u1({group}), req_body);

        auto [ec, rsp] = co_await host.async_request(
            1,
            3,
            bytes_view{req_body.data(), req_body.size()},
            1s);
        if (ec) {
            std::cout << "Host S1F3 request(group=" << static_cast<int>(group)
                      << ") failed: " << ec.message() << "\n";
        } else {
            (void)decode_and_print_item("[Host] recv", rsp);
        }
    }

    host.stop();
    equip.stop();
    (void)co_await equip_done.async_wait(1s);
    std::cout << "demo1 done\n";
}

// demo2：Equipment 主动发起 request，Host 侧回包。
static asio::awaitable<void> demo_equipment_requests_host() {
    auto ex = co_await asio::this_coro::executor;

    std::cout << "\n=== demo2: Equipment -> Host (S2F1) ===\n";

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 200ms;
    timeouts.t3_reply = 1s;
    timeouts.t4_interblock = 200ms;

    constexpr std::uint16_t device_id = 1;
    auto [host_link, equip_link] = MemoryLink::create(ex);

    StateMachine host_sm(host_link, device_id, timeouts);
    StateMachine equip_sm(equip_link, device_id, timeouts);

    SessionOptions host_opt{};
    host_opt.t3 = 1s;
    host_opt.poll_interval = 10ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment

    SessionOptions equip_opt = host_opt;
    equip_opt.secs1_reverse_bit = true; // Equipment -> Host

    Session host(host_sm, device_id, host_opt);
    Session equip(equip_sm, device_id, equip_opt);

    // Host 侧：注册一个 handler，用于响应 Equipment 发来的 S2F1。
    host.router().set(
        2,
        1,
        [](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            // 示例：回一个简单的 ASCII("OK")（secondary S2F2）
            (void)req;
            auto [ec, body] = encode_item(Item::ascii("OK"));
            co_return HandlerResult{ec, std::move(body)};
        });

    // Host 侧常驻接收循环（负责接收 Equipment primary 并回包）。
    secs::core::Event host_done{};
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await host.async_run(); },
        [&](std::exception_ptr) { host_done.set(); });

    // Equipment 主动发起 request：S2F1(W=1)
    {
        std::vector<byte> req_body;
        (void)secs::ii::encode(Item::ascii("ping"), req_body);

        auto [ec, rsp] = co_await equip.async_request(
            2,
            1,
            bytes_view{req_body.data(), req_body.size()},
            1s);
        if (ec) {
            std::cout << "Equipment S2F1 request failed: " << ec.message()
                      << "\n";
        } else {
            (void)decode_and_print_item("[Equipment] recv", rsp);
        }
    }

    host.stop();
    equip.stop();
    (void)co_await host_done.async_wait(1s);
    std::cout << "demo2 done\n";
}

} // namespace

int main() {
    // 默认关闭库内部 debug 日志（需要排查问题时可改为 debug/trace）。
    secs::core::set_log_level(secs::core::LogLevel::off);

    asio::io_context ioc;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            co_await demo_host_requests_equipment();
            co_await demo_equipment_requests_host();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return 0;
}

