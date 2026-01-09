/**
 * @file smlx_active_send_example.cpp
 * @brief SMLX 示例：占位符渲染 + 在代码中主动发送（基于 SECS-I MemoryLink 回环）
 *
 * 说明：
 * - 本示例不依赖真实串口/网络，跨平台可运行；
 * - Host 侧使用 `secs::sml::Runtime::encode_message_body()` 把 SMLX 模板渲染为
 *   可直接发送的 SECS-II body bytes；
 * - Equipment 侧注册 Router handler，并用同一份 SMLX 模板生成回应 body；
 * - 示例关注点是 API 用法演示，不代表某条 SxFy 的标准语义。
 *
 * 用法：
 *   ./smlx_active_send_example
 */

#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace secs;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kDeviceId = 1;

static void dump_indent(int n) {
    for (int i = 0; i < n; ++i) {
        std::cout << ' ';
    }
}

static void dump_bytes(const std::vector<ii::byte> &bytes) {
    std::ios old_state(nullptr);
    old_state.copyfmt(std::cout);

    std::cout << std::hex << std::uppercase << std::setfill('0');
    for (auto b : bytes) {
        std::cout << " 0x" << std::setw(2) << static_cast<int>(b);
    }

    std::cout.copyfmt(old_state);
}

static void dump_item(const ii::Item &item, int indent = 0) {
    if (const auto *list = item.get_if<ii::List>()) {
        dump_indent(indent);
        std::cout << "<L> size=" << list->size() << "\n";
        for (std::size_t i = 0; i < list->size(); ++i) {
            dump_indent(indent);
            std::cout << "- [" << i << "]\n";
            dump_item((*list)[i], indent + 2);
        }
        return;
    }

    if (const auto *a = item.get_if<ii::ASCII>()) {
        dump_indent(indent);
        std::cout << "<A \"" << a->value << "\">\n";
        return;
    }

    if (const auto *b = item.get_if<ii::Binary>()) {
        dump_indent(indent);
        std::cout << "<B> count=" << b->value.size() << " bytes:";
        dump_bytes(b->value);
        std::cout << "\n";
        return;
    }

    if (const auto *b = item.get_if<ii::Boolean>()) {
        dump_indent(indent);
        std::cout << "<Boolean> count=" << b->values.size() << " values:";
        for (bool v : b->values) {
            std::cout << ' ' << (v ? 1 : 0);
        }
        std::cout << "\n";
        return;
    }

    if (const auto *v = item.get_if<ii::I1>()) {
        dump_indent(indent);
        std::cout << "<I1> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << static_cast<int>(x);
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::I2>()) {
        dump_indent(indent);
        std::cout << "<I2> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::I4>()) {
        dump_indent(indent);
        std::cout << "<I4> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::I8>()) {
        dump_indent(indent);
        std::cout << "<I8> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }

    if (const auto *v = item.get_if<ii::U1>()) {
        dump_indent(indent);
        std::cout << "<U1> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << static_cast<unsigned>(x);
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::U2>()) {
        dump_indent(indent);
        std::cout << "<U2> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::U4>()) {
        dump_indent(indent);
        std::cout << "<U4> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::U8>()) {
        dump_indent(indent);
        std::cout << "<U8> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }

    if (const auto *v = item.get_if<ii::F4>()) {
        dump_indent(indent);
        std::cout << "<F4> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }
    if (const auto *v = item.get_if<ii::F8>()) {
        dump_indent(indent);
        std::cout << "<F8> count=" << v->values.size() << " values:";
        for (auto x : v->values) {
            std::cout << ' ' << x;
        }
        std::cout << "\n";
        return;
    }

    dump_indent(indent);
    std::cout << "(示例：未对该 Item 类型做格式化输出)\n";
}

static void dump_decoded_item(std::string_view prefix, const ii::Item &decoded) {
    std::cout << prefix << "\n";
    dump_item(decoded, 2);
}

static std::pair<std::error_code, ii::Item>
decode_body(core::bytes_view body) {
    ii::Item decoded{ii::List{}};
    std::size_t consumed = 0;
    const auto ec = ii::decode_one(body, decoded, consumed);
    if (ec) {
        return {ec, ii::Item::list({})};
    }
    if (consumed != body.size()) {
        return {core::make_error_code(core::errc::invalid_argument),
                ii::Item::list({})};
    }
    return {std::error_code{}, std::move(decoded)};
}

asio::awaitable<int> run() {
    auto ex = co_await asio::this_coro::executor;

    // 1) 创建一对“内存互联”的 SECS-I Link 端点
    auto [host_link, eq_link] = secs::secs1::MemoryLink::create(ex);

    // 2) 在两端分别创建 SECS-I 传输层状态机
    secs::secs1::StateMachine host_sm(host_link, kDeviceId);
    secs::secs1::StateMachine eq_sm(eq_link, kDeviceId);

    // 3) 在两端创建统一协议层 Session（注意 R-bit 方向配置）
    protocol::SessionOptions host_opt{};
    host_opt.t3 = 3s;
    host_opt.poll_interval = 20ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment（R=0）

    protocol::SessionOptions eq_opt = host_opt;
    eq_opt.secs1_reverse_bit = true; // Equipment -> Host（R=1）

    protocol::Session host_sess(host_sm, kDeviceId, host_opt);
    protocol::Session eq_sess(eq_sm, kDeviceId, eq_opt);

    // 4) 加载一份 SMLX 模板：
    // - req_all：覆盖所有占位符类型与“数组展开拼接”语义
    // - rsp_all：也用占位符生成响应，验证“模板渲染可用于回包”
    secs::sml::Runtime rt;
    const auto ec = rt.load(R"(
req_all: S1F13 W
<L
  <A MDLN>
  <B 0x01 BYTES 255>
  <Boolean 0 BOOLS 1>
  <I1 -1 I1S 2>
  <I2 -2 I2S 3>
  <I4 -4 I4S 5>
  <I8 -8 I8S 13>
  <U1 1 U1S 3>
  <U2 10 U2S 30>
  <U4 100 U4S 300>
  <U8 1000 U8S 3000>
  <F4 1 F4S 2>
  <F8 1 F8S 2>
  <L <A HOSTREV> <U2 HOST_SVIDS>>
>.

rsp_all: S1F14
<L
  <A SOFTREV>
  <U2 0 CEIDS 65535>
  <B RSP_BYTES>
>.
)");
    if (ec) {
        std::cerr << "[SMLX] load failed: " << ec.message() << "\n";
        co_return 1;
    }

    // 4.1) 演示渲染错误分支：缺少变量 / 类型不匹配
    {
        std::vector<core::byte> out_body{0xAA, 0xBB};
        secs::sml::RenderContext empty{};
        const auto e = rt.encode_message_body("req_all", empty, out_body);
        std::cout << "[Host] expected render error (missing_variable): "
                  << e.category().name() << "/" << e.value() << " "
                  << e.message() << " (body_n=" << out_body.size() << ")\n";
    }
    {
        std::vector<core::byte> out_body{0xAA, 0xBB};
        secs::sml::RenderContext bad{};
        bad.set("MDLN", ii::Item::u2(std::vector<std::uint16_t>{1}));
        const auto e = rt.encode_message_body("req_all", bad, out_body);
        std::cout << "[Host] expected render error (type_mismatch): "
                  << e.category().name() << "/" << e.value() << " "
                  << e.message() << " (body_n=" << out_body.size() << ")\n";
    }

    // Equipment 侧：收到 S1F13 后，打印请求内容，并用 SMLX 模板生成回应 body。
    eq_sess.router().set(
        1,
        13,
        [&rt](const protocol::DataMessage &req)
            -> asio::awaitable<protocol::HandlerResult> {
            if (!req.body.empty()) {
                auto [dec_ec, decoded] = decode_body(
                    core::bytes_view{req.body.data(), req.body.size()});
                if (dec_ec) {
                    std::cout << "[Equip] decode request failed: "
                              << dec_ec.message() << "\n";
                } else {
                    dump_decoded_item("[Equip] decoded request: ", decoded);
                }
            }

            secs::sml::RenderContext ctx;
            ctx.set("SOFTREV", ii::Item::ascii("REV.01"));
            ctx.set("CEIDS",
                    ii::Item::u2(std::vector<std::uint16_t>{4001, 4002}));
            ctx.set("RSP_BYTES",
                    ii::Item::binary(std::vector<ii::byte>{0xAA, 0xBB}));

            std::vector<core::byte> rsp_body;
            const auto enc_ec = rt.encode_message_body("rsp_all", ctx, rsp_body);
            if (enc_ec) {
                std::cout << "[Equip] encode rsp failed: " << enc_ec.message()
                          << " (fallback to empty)\n";
                rsp_body.clear();
            }

            co_return protocol::HandlerResult{std::error_code{},
                                              std::move(rsp_body)};
        });

    // Equipment 侧：启动接收循环（负责收包、路由 handler、回包）
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await eq_sess.async_run(); },
        asio::detached);

    // 5) Host 侧：注入变量并渲染 req_all，随后根据 W-bit 决定 async_send/async_request。
    secs::sml::RenderContext host_ctx;
    host_ctx.set("MDLN", ii::Item::ascii("WET.01"));
    host_ctx.set("BYTES",
                 ii::Item::binary(std::vector<ii::byte>{0x02, 0x03}));
    host_ctx.set("BOOLS", ii::Item::boolean(std::vector<bool>{true, false, true}));

    host_ctx.set("I1S",
                 ii::Item::i1(std::vector<std::int8_t>{-128, 127}));
    host_ctx.set("I2S",
                 ii::Item::i2(std::vector<std::int16_t>{-300, 400}));
    host_ctx.set("I4S",
                 ii::Item::i4(std::vector<std::int32_t>{-100000, 100000}));
    host_ctx.set("I8S",
                 ii::Item::i8(std::vector<std::int64_t>{-5000000000LL, 5000000000LL}));

    host_ctx.set("U1S", ii::Item::u1(std::vector<std::uint8_t>{0, 255}));
    host_ctx.set("U2S", ii::Item::u2(std::vector<std::uint16_t>{100, 200}));
    host_ctx.set("U4S", ii::Item::u4(std::vector<std::uint32_t>{100000, 200000}));
    host_ctx.set("U8S", ii::Item::u8(std::vector<std::uint64_t>{10000000000ULL, 20000000000ULL}));

    host_ctx.set("F4S", ii::Item::f4(std::vector<float>{3.5f, -1.25f}));
    host_ctx.set("F8S", ii::Item::f8(std::vector<double>{2.25, -0.5}));

    host_ctx.set("HOSTREV", ii::Item::ascii("HOST.REV"));
    host_ctx.set("HOST_SVIDS",
                 ii::Item::u2(std::vector<std::uint16_t>{3001, 3002}));

    std::vector<core::byte> req_body;
    std::uint8_t stream = 0;
    std::uint8_t function = 0;
    bool w_bit = false;
    const auto enc_ec = rt.encode_message_body(
        "req_all", host_ctx, req_body, &stream, &function, &w_bit);
    if (enc_ec) {
        std::cerr << "[Host] encode req failed: " << enc_ec.message() << "\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 2;
    }

    // 本地验证：先解码一次，确认“模板替代”生效。
    {
        auto [dec_ec, decoded] = decode_body(
            core::bytes_view{req_body.data(), req_body.size()});
        if (dec_ec) {
            std::cerr << "[Host] decode req failed: " << dec_ec.message() << "\n";
            host_sess.stop();
            eq_sess.stop();
            co_return 3;
        }

        const ii::Item expected = ii::Item::list({
            ii::Item::ascii("WET.01"),
            ii::Item::binary(std::vector<ii::byte>{0x01, 0x02, 0x03, 255}),
            ii::Item::boolean(std::vector<bool>{false, true, false, true, true}),
            ii::Item::i1(std::vector<std::int8_t>{-1, -128, 127, 2}),
            ii::Item::i2(std::vector<std::int16_t>{-2, -300, 400, 3}),
            ii::Item::i4(std::vector<std::int32_t>{-4, -100000, 100000, 5}),
            ii::Item::i8(std::vector<std::int64_t>{-8, -5000000000LL, 5000000000LL, 13}),
            ii::Item::u1(std::vector<std::uint8_t>{1, 0, 255, 3}),
            ii::Item::u2(std::vector<std::uint16_t>{10, 100, 200, 30}),
            ii::Item::u4(std::vector<std::uint32_t>{100, 100000, 200000, 300}),
            ii::Item::u8(std::vector<std::uint64_t>{1000ULL, 10000000000ULL, 20000000000ULL, 3000ULL}),
            ii::Item::f4(std::vector<float>{1.0f, 3.5f, -1.25f, 2.0f}),
            ii::Item::f8(std::vector<double>{1.0, 2.25, -0.5, 2.0}),
            ii::Item::list({
                ii::Item::ascii("HOST.REV"),
                ii::Item::u2(std::vector<std::uint16_t>{3001, 3002}),
            }),
        });

        if (!(decoded == expected)) {
            std::cerr << "[Host] rendered request mismatch\n";
            dump_decoded_item("[Host] decoded request:", decoded);
            host_sess.stop();
            eq_sess.stop();
            co_return 4;
        }

        dump_decoded_item("[Host] decoded request (render OK):", decoded);
    }

    std::cout << "[Host] active send: S" << static_cast<int>(stream) << "F"
              << static_cast<int>(function) << " (W=" << w_bit
              << "), body=" << req_body.size() << " bytes\n";

    const core::bytes_view req_view{req_body.data(), req_body.size()};
    if (!w_bit) {
        const auto send_ec =
            co_await host_sess.async_send(stream, function, req_view);
        if (send_ec) {
            std::cerr << "[Host] async_send failed: " << send_ec.message()
                      << "\n";
            host_sess.stop();
            eq_sess.stop();
            co_return 3;
        }
        std::cout << "[Host] send OK (W=0)\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 0;
    }

    auto [req_ec, rsp] =
        co_await host_sess.async_request(stream, function, req_view, 5s);
    if (req_ec) {
        std::cerr << "[Host] async_request failed: " << req_ec.message()
                  << "\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 4;
    }

    std::cout << "[Host] got response: S" << static_cast<int>(rsp.stream)
              << "F" << static_cast<int>(rsp.function) << " (W=" << rsp.w_bit
              << "), body=" << rsp.body.size() << " bytes\n";

    // 6) Host 侧：解码回应并打印（验证“响应也可由模板+变量生成”）。
    if (!rsp.body.empty()) {
        auto [dec_ec, decoded] = decode_body(
            core::bytes_view{rsp.body.data(), rsp.body.size()});
        if (dec_ec) {
            std::cerr << "[Host] decode rsp failed: " << dec_ec.message()
                      << "\n";
            host_sess.stop();
            eq_sess.stop();
            co_return 5;
        }

        const ii::Item expected = ii::Item::list({
            ii::Item::ascii("REV.01"),
            ii::Item::u2(std::vector<std::uint16_t>{0, 4001, 4002, 65535}),
            ii::Item::binary(std::vector<ii::byte>{0xAA, 0xBB}),
        });
        if (!(decoded == expected)) {
            std::cerr << "[Host] rendered response mismatch\n";
            dump_decoded_item("[Host] decoded response:", decoded);
            host_sess.stop();
            eq_sess.stop();
            co_return 6;
        }

        dump_decoded_item("[Host] decoded response (render OK):", decoded);
    }

    std::cout << "PASS\n";
    host_sess.stop();
    eq_sess.stop();
    co_return 0;
}

} // namespace

int main() {
    std::cout << "=== SMLX Active Send Example (SECS-I MemoryLink) ===\n\n";

    asio::io_context ioc;
    int rc = 1;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            rc = co_await run();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
