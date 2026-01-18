#include "secs/sml/runtime.hpp"

#include "secs/ii/codec.hpp"
#include "secs/sml/render.hpp"

#include "test_main.hpp"

namespace {

void test_sf_index_named_first_wins() {
    secs::sml::Runtime rt;
    const char *source = R"(
m1: S1F1 W <L <A "first">>.
m2: S1F1 W <L <A "second">>.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    const auto *msg = rt.get_message(1, 1);
    TEST_EXPECT(msg != nullptr);
    TEST_EXPECT_EQ(msg->name, "m1");
}

void test_sf_index_anonymous_overrides_named() {
    secs::sml::Runtime rt;
    const char *source = R"(
m1: S1F1 W <L <A "named">>.
S1F1 W <L <A "anon">>.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    const auto *msg_sf = rt.get_message(1, 1);
    TEST_EXPECT(msg_sf != nullptr);
    TEST_EXPECT(msg_sf->name.empty());

    const auto *msg_name = rt.get_message("S1F1");
    TEST_EXPECT(msg_name != nullptr);
    TEST_EXPECT(msg_name->name.empty());
}

void test_match_response_list_index_selection() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<U2 1>) r0.
if (S2F21[1]==<U2 2>) r1.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    {
        const auto body = secs::ii::Item::list({
            secs::ii::Item::u2({1}),
            secs::ii::Item::u2({2}),
        });
        const auto rsp = rt.match_response(2, 21, body);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r0");
    }

    {
        const auto body = secs::ii::Item::list({
            secs::ii::Item::u2({0}),
            secs::ii::Item::u2({2}),
        });
        const auto rsp = rt.match_response(2, 21, body);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r1");
    }
}

void test_match_response_preorder_index_compat() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21(4)==<U1 1>) ok.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    const auto body = secs::ii::Item::list({
        secs::ii::Item::ascii("a"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1}),
            secs::ii::Item::u1({2}),
        }),
    });

    const auto rsp = rt.match_response(2, 21, body);
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, "ok");
}

void test_match_response_with_trace_success_has_no_traces() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<U2 2>) r0.
if (S2F21[0]==<U2 1>) r1.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::list({secs::ii::Item::u2({1})});

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(result.response_name.has_value());
    TEST_EXPECT_EQ(*result.response_name, "r1");
    TEST_EXPECT(result.traces.empty());
}

void test_match_response_with_trace_collects_failures() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<U2 2>) r0.
if (S2F21[1]==<U2 2>) r1.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::list({secs::ii::Item::u2({1})});

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 2u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::expected_value_mismatch);
    TEST_EXPECT_EQ(result.traces[1].reason,
                   secs::sml::MatchFailureReason::list_index_out_of_bounds);
    TEST_EXPECT(!result.traces[1].detail.empty());
}

void test_match_response_with_trace_not_a_list() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<U2 1>) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::u2({1}); // 非 List

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::not_a_list);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_match_response_with_trace_index_out_of_bounds() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21(99)==<U2 1>) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::list({secs::ii::Item::u2({1})});

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::index_out_of_bounds);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_match_response_with_trace_expected_placeholder_rendering() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<U2 CEID>) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    const auto body = secs::ii::Item::list({secs::ii::Item::u2({0x1001})});

    // 1) 缺失变量 -> 渲染失败
    {
        secs::sml::RenderContext ctx;
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 1u);
        TEST_EXPECT_EQ(result.traces[0].reason,
                       secs::sml::MatchFailureReason::render_missing_variable);
    }

    // 2) 类型不匹配 -> 渲染失败
    {
        secs::sml::RenderContext ctx;
        ctx.set("CEID", secs::ii::Item::ascii("0x1001"));
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 1u);
        TEST_EXPECT_EQ(result.traces[0].reason,
                       secs::sml::MatchFailureReason::render_type_mismatch);
    }

    // 3) 渲染成功且匹配命中
    {
        secs::sml::RenderContext ctx;
        ctx.set("CEID", secs::ii::Item::u2({0x1001}));
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(result.response_name.has_value());
        TEST_EXPECT_EQ(*result.response_name, "r0");
        TEST_EXPECT(result.traces.empty());

        const auto rsp = rt.match_response(2, 21, body, ctx);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r0");
    }
}

void test_match_response_with_trace_stream_function_mismatch() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21) r0.
if (request) r1.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::list({});

    const auto result = rt.match_response_with_trace(1, 1, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 2u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::stream_function_mismatch);
    TEST_EXPECT_EQ(result.traces[1].reason,
                   secs::sml::MatchFailureReason::stream_function_mismatch);
    TEST_EXPECT(!result.traces[0].detail.empty());
    TEST_EXPECT(!result.traces[1].detail.empty());
}

void test_match_response_with_trace_named_message_mismatch() {
    secs::sml::Runtime rt;
    const char *source = R"(
m: S2F21 <L>.
if (m) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::list({});

    const auto result = rt.match_response_with_trace(1, 1, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::stream_function_mismatch);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_match_response_with_trace_preorder_index_on_non_list() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21(2)==<U2 1>) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;
    const auto body = secs::ii::Item::u2({1}); // 非 List

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::index_out_of_bounds);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_runtime_accessors_and_parse_error() {
    // 覆盖 inline accessor（runtime.hpp）以及 parse_sml() 的 lexer 错误分支。
    {
        const auto r = secs::sml::parse_sml("@");
        TEST_EXPECT(r.ec.value() != 0);
        TEST_EXPECT(!r.error_message.empty());
    }

    secs::sml::Runtime rt;
    TEST_EXPECT(!rt.loaded());

    {
        const auto ec = rt.load("@");
        TEST_EXPECT(ec.value() != 0);
        TEST_EXPECT(!rt.loaded());
    }

    const char *source = R"(
m: S1F13 W <L <U2 1>>.
if (S1F1) m.
)";
    {
        const auto ec = rt.load(source);
        TEST_EXPECT_OK(ec);
        TEST_EXPECT(rt.loaded());
        TEST_EXPECT_EQ(rt.messages().size(), 1u);
        TEST_EXPECT_EQ(rt.conditions().size(), 1u);
        TEST_EXPECT_EQ(rt.timers().size(), 0u);
    }
}

void test_encode_message_body() {
    secs::sml::Runtime rt;
    const char *source = R"(
m: S1F13 W <L <U2 CEID>>.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    std::vector<secs::core::byte> body;
    std::uint8_t stream = 0;
    std::uint8_t function = 0;
    bool w = false;

    // 1) 缺失变量 -> 渲染失败
    {
        secs::sml::RenderContext ctx;
        const auto enc_ec =
            rt.encode_message_body("m", ctx, body, &stream, &function, &w);
        TEST_EXPECT(enc_ec == secs::sml::render_errc::missing_variable);
    }

    // 2) 成功渲染并编码
    {
        secs::sml::RenderContext ctx;
        ctx.set("CEID", secs::ii::Item::u2({0x1001}));

        const auto enc_ec =
            rt.encode_message_body("m", ctx, body, &stream, &function, &w);
        TEST_EXPECT_OK(enc_ec);
        TEST_EXPECT_EQ(stream, 1u);
        TEST_EXPECT_EQ(function, 13u);
        TEST_EXPECT(w);
        TEST_EXPECT(!body.empty());

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        const auto dec_ec = secs::ii::decode_one(
            secs::ii::bytes_view{body.data(), body.size()}, decoded, consumed);
        TEST_EXPECT_OK(dec_ec);
        TEST_EXPECT_EQ(consumed, body.size());

        const auto expected =
            secs::ii::Item::list({secs::ii::Item::u2({0x1001})});
        TEST_EXPECT(decoded == expected);
    }

    // 3) 未知消息 -> invalid_argument
    {
        secs::sml::RenderContext ctx;
        const auto enc_ec = rt.encode_message_body("not_found", ctx, body);
        TEST_EXPECT(enc_ec == secs::core::errc::invalid_argument);
    }
}

void test_items_equal_float_tolerance() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0]==<F4 1.0>) f4_ok.
if (S2F21[0]==<F8 1.0>) f8_ok.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    // 1) F4：容差范围内应命中
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f4({1.00005f})});
        const auto rsp = rt.match_response(2, 21, body, ctx);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "f4_ok");
    }

    // 2) F4：长度不一致 -> 不命中
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f4({1.0f, 2.0f})});
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 2u);
        TEST_EXPECT_EQ(result.traces[0].reason,
                       secs::sml::MatchFailureReason::expected_value_mismatch);
    }

    // 2.1) F4：超出容差 -> 不命中（覆盖 float_almost_equal() 的 false 分支）
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f4({1.0005f})});
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 2u);
        TEST_EXPECT_EQ(result.traces[0].reason,
                       secs::sml::MatchFailureReason::expected_value_mismatch);
    }

    // 3) F8：容差范围内应命中
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f8({1.00005})});
        const auto rsp = rt.match_response(2, 21, body, ctx);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "f8_ok");
    }

    // 4) F8：超出容差 -> 不命中
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f8({1.0005})});
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 2u);
        TEST_EXPECT_EQ(result.traces[1].reason,
                       secs::sml::MatchFailureReason::expected_value_mismatch);
    }

    // 4.1) F8：长度不一致 -> 不命中（覆盖 size mismatch 分支）
    {
        secs::sml::RenderContext ctx;
        const auto body =
            secs::ii::Item::list({secs::ii::Item::f8({1.0, 2.0})});
        const auto result = rt.match_response_with_trace(2, 21, body, ctx);
        TEST_EXPECT(!result.response_name.has_value());
        TEST_EXPECT_EQ(result.traces.size(), 2u);
        TEST_EXPECT_EQ(result.traces[1].reason,
                       secs::sml::MatchFailureReason::expected_value_mismatch);
    }
}

} // namespace

int main() {
    test_sf_index_named_first_wins();
    test_sf_index_anonymous_overrides_named();
    test_match_response_list_index_selection();
    test_match_response_preorder_index_compat();
    test_match_response_with_trace_success_has_no_traces();
    test_match_response_with_trace_collects_failures();
    test_match_response_with_trace_not_a_list();
    test_match_response_with_trace_index_out_of_bounds();
    test_match_response_with_trace_expected_placeholder_rendering();
    test_match_response_with_trace_stream_function_mismatch();
    test_match_response_with_trace_named_message_mismatch();
    test_match_response_with_trace_preorder_index_on_non_list();
    test_runtime_accessors_and_parse_error();
    test_encode_message_body();
    test_items_equal_float_tolerance();
    return secs::tests::run_and_report();
}
