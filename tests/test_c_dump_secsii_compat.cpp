/**
 * @file test_c_dump_secsii_compat.cpp
 * @brief 对齐 c_dump 参考实现的 SECS-II 编码（字节级一致性）
 *
 * 说明：
 * - c_dump/Secs_App/secs_II.c 在真实环境中跑通过，这里把它当作“参考输出”。
 * - 本测试将同一条 <...> 语法的 Item 文本：
 *   1) 交给 c_dump 的 Secs_MessageArrange 生成字节流（期望值）
 *   2) 交给本库 sml 解析器生成 ii::Item，再由 ii::encode 编码（实际值）
 * - 两者必须逐字节一致，否则说明库内实现仅“自洽”但不兼容真实环境。
 */

#include "secs/ii/codec.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"

#include "test_main.hpp"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "secs_II.h"
}

namespace {

std::vector<std::uint8_t> encode_with_c_dump(std::string item_text) {
    SendDataInfo info{};
    const int rc = Secs_MessageArrange(item_text.data(), &info);
    TEST_EXPECT_EQ(rc, 0);

    std::vector<std::uint8_t> out;
    if (info.SendData_Buf != nullptr && info.SendData_Size > 0) {
        out.assign(info.SendData_Buf, info.SendData_Buf + info.SendData_Size);
    }

    if (info.SendData_Buf) {
        free(info.SendData_Buf);
        info.SendData_Buf = nullptr;
    }
    return out;
}

std::vector<std::uint8_t> encode_with_cpp(std::string_view item_text) {
    // sml 解析器需要一个完整的“消息定义”，这里用匿名消息包裹。
    std::string sml;
    sml.reserve(item_text.size() + 16);
    sml += "S1F1 ";
    sml.append(item_text);
    sml += ".";

    const auto parsed = secs::sml::parse_sml(sml);
    TEST_EXPECT_OK(parsed.ec);
    TEST_EXPECT_EQ(parsed.document.messages.size(), 1u);

    const auto &msg = parsed.document.messages[0];
    secs::sml::RenderContext ctx{};
    secs::ii::Item rendered{secs::ii::List{}};
    TEST_EXPECT_OK(secs::sml::render_item(msg.item, ctx, rendered));

    std::vector<secs::ii::byte> encoded;
    TEST_EXPECT_OK(secs::ii::encode(rendered, encoded));

    std::vector<std::uint8_t> out;
    out.reserve(encoded.size());
    for (auto b : encoded) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

void expect_same_encoding(std::string_view item_text) {
    const auto expected = encode_with_c_dump(std::string(item_text));
    const auto actual = encode_with_cpp(item_text);

    // 先做整体比较；若失败，再输出第一个差异位置，便于定位。
    if (expected == actual) {
        TEST_EXPECT(true);
        return;
    }

    TEST_FAIL("c_dump 编码与本库编码不一致");
    TEST_EXPECT_EQ(expected.size(), actual.size());

    const std::size_t n = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (expected[i] != actual[i]) {
            // 只报告第一个差异点，避免刷屏。
            std::string msg;
            msg.reserve(96);
            msg += "首个差异字节位置: ";
            msg += std::to_string(i);
            msg += " expected=0x";
            {
                char buf[3];
                std::snprintf(buf, sizeof(buf), "%02X", expected[i]);
                msg += buf;
            }
            msg += " actual=0x";
            {
                char buf[3];
                std::snprintf(buf, sizeof(buf), "%02X", actual[i]);
                msg += buf;
            }
            TEST_FAIL(msg);
            break;
        }
    }
}

void test_c_dump_compat_basic_items() {
    expect_same_encoding(R"(<A "Hello">)");
    expect_same_encoding(R"(<U1 7>)");
    expect_same_encoding(R"(<U2 123>)");
    expect_same_encoding(R"(<U4 123456>)");
    expect_same_encoding(R"(<I1 -1>)");
    expect_same_encoding(R"(<I2 -1234>)");
    expect_same_encoding(R"(<I4 -12345>)");
    expect_same_encoding(R"(<F4 3.14159>)");
}

void test_c_dump_compat_nested_list() {
    expect_same_encoding(
        R"(<L[4]<A "Hello"><I4 -12345><U4 123456><F4 3.14159>>)");
}

void test_c_dump_compat_tvoc_like_payload() {
    expect_same_encoding(
        R"(<L[2]<A "project: tvoc_secs"><A "version: 25-09-28">>)");
}

} // namespace

int main() {
    test_c_dump_compat_basic_items();
    test_c_dump_compat_nested_list();
    test_c_dump_compat_tvoc_like_payload();
    return secs::tests::run_and_report();
}
