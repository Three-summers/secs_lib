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

void test_match_response_deep_path_indexing_selection() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[1][0]==<U1 7>) ok.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    // body: <L <U2 1> <L <U1 7> <U1 8> > >
    const auto body = secs::ii::Item::list({
        secs::ii::Item::u2({1}),
        secs::ii::Item::list({
            secs::ii::Item::u1({7}),
            secs::ii::Item::u1({8}),
        }),
    });

    const auto rsp = rt.match_response(2, 21, body);
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, "ok");
}

void test_match_response_deep_path_indexing_not_a_list() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0][0]==<U1 7>) ok.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;

    // root[0] 不是 List，因此第二层索引应触发 not_a_list。
    const auto body = secs::ii::Item::list({
        secs::ii::Item::u2({1}),
        secs::ii::Item::u1({7}),
    });

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::not_a_list);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_match_response_deep_path_indexing_out_of_bounds() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S2F21[0][1]==<U1 7>) ok.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    secs::sml::RenderContext ctx;

    // root[0] 是 List，但 root[0][1] 越界。
    const auto body = secs::ii::Item::list({
        secs::ii::Item::list({
            secs::ii::Item::u1({7}),
        }),
    });

    const auto result = rt.match_response_with_trace(2, 21, body, ctx);
    TEST_EXPECT(!result.response_name.has_value());
    TEST_EXPECT_EQ(result.traces.size(), 1u);
    TEST_EXPECT_EQ(result.traces[0].reason,
                   secs::sml::MatchFailureReason::list_index_out_of_bounds);
    TEST_EXPECT(!result.traces[0].detail.empty());
}

void test_match_response_with_capture_pattern() {
    secs::sml::Runtime rt;
    const char *source = R"(
r0: S2F22 <L <U2 CAP_A>>.
if (S2F21 <L [2] <U2 $CAP_A> <L $CAP_B>>) r0.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    const auto body = secs::ii::Item::list({
        secs::ii::Item::u2({0x1001}),
        secs::ii::Item::list({secs::ii::Item::ascii("x")}),
    });

    // 1) 仅匹配（不关心 capture）
    {
        const auto rsp = rt.match_response(2, 21, body);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r0");
    }

    // 2) 匹配 + 捕获
    secs::sml::RenderContext captured;
    const auto rsp = rt.match_response_with_capture(2, 21, body, captured);
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, "r0");

    {
        const auto *a = captured.get("CAP_A");
        TEST_EXPECT(a != nullptr);
        const auto *u2 = a->get_if<secs::ii::U2>();
        TEST_EXPECT(u2 != nullptr);
        TEST_EXPECT_EQ(u2->values.size(), 1u);
        TEST_EXPECT_EQ(u2->values[0], 0x1001u);
    }

    {
        const auto *b = captured.get("CAP_B");
        TEST_EXPECT(b != nullptr);
        const auto *list = b->get_if<secs::ii::List>();
        TEST_EXPECT(list != nullptr);
        TEST_EXPECT_EQ(list->size(), 1u);
        const auto *ascii = (*list)[0].get_if<secs::ii::ASCII>();
        TEST_EXPECT(ascii != nullptr);
        TEST_EXPECT_EQ(ascii->value, "x");
    }

    // 3) 捕获上下文可用于渲染响应模板（配置即解析）
    std::vector<secs::core::byte> out_body;
    std::uint8_t out_stream = 0;
    std::uint8_t out_function = 0;
    bool out_w = true;
    const auto enc_ec =
        rt.encode_message_body("r0", captured, out_body, &out_stream, &out_function, &out_w);
    TEST_EXPECT_OK(enc_ec);
    TEST_EXPECT_EQ(out_stream, 2u);
    TEST_EXPECT_EQ(out_function, 22u);
    TEST_EXPECT(!out_w);

    secs::ii::Item decoded{secs::ii::List{}};
    std::size_t consumed = 0;
    const auto dec_ec = secs::ii::decode_one(
        secs::ii::bytes_view{out_body.data(), out_body.size()}, decoded, consumed);
    TEST_EXPECT_OK(dec_ec);
    TEST_EXPECT_EQ(consumed, out_body.size());
    TEST_EXPECT(decoded ==
                secs::ii::Item::list({secs::ii::Item::u2({0x1001})}));
}

void test_match_response_with_capture_pattern_various_types() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S1F1 <A $CAP_A>) r_ascii.
if (S1F3 <B 0x01 0x02>) r_bin.
if (S1F5 <Boolean 0 1 0>) r_bool.
if (S1F7 <I2 -1 2>) r_i2.
if (S1F9 <U4 0x10000000>) r_u4.
if (S1F11 <F4 1.0>) r_f4.
if (S1F13 <F8 1.0>) r_f8.
if (S1F15 <L [2] <U1 1> <U1 2>>) r_list.
if (S1F17 <L [2] $CAP_L>) r_cap_list.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    // ASCII + capture
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::ascii("hello");
        const auto rsp = rt.match_response_with_capture(1, 1, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_ascii");
        const auto *v = cap.get("CAP_A");
        TEST_EXPECT(v != nullptr);
        const auto *a = v->get_if<secs::ii::ASCII>();
        TEST_EXPECT(a != nullptr);
        TEST_EXPECT_EQ(a->value, "hello");
    }

    // Binary literal match
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::binary(
            std::vector<secs::ii::byte>{static_cast<secs::ii::byte>(1),
                                        static_cast<secs::ii::byte>(2)});
        const auto rsp = rt.match_response_with_capture(1, 3, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_bin");
        TEST_EXPECT(cap.get("NO_SUCH") == nullptr);
    }

    // Boolean literal match
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::boolean(
            std::vector<bool>{false, true, false});
        const auto rsp = rt.match_response_with_capture(1, 5, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_bool");
    }

    // Signed
    {
        secs::sml::RenderContext cap;
        const auto body =
            secs::ii::Item::i2(std::vector<std::int16_t>{-1, 2});
        const auto rsp = rt.match_response_with_capture(1, 7, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_i2");
    }

    // Unsigned
    {
        secs::sml::RenderContext cap;
        const auto body =
            secs::ii::Item::u4(std::vector<std::uint32_t>{0x10000000u});
        const auto rsp = rt.match_response_with_capture(1, 9, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_u4");
    }

    // Float tolerance (F4): abs diff <= 0.0001
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::f4(std::vector<float>{1.00005f});
        const auto rsp = rt.match_response_with_capture(1, 11, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_f4");
    }
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::f4(std::vector<float>{1.0002f});
        const auto rsp = rt.match_response_with_capture(1, 11, body, cap);
        TEST_EXPECT(!rsp.has_value());
    }

    // Float tolerance (F8)
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::f8(std::vector<double>{1.00005});
        const auto rsp = rt.match_response_with_capture(1, 13, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_f8");
    }
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::f8(std::vector<double>{1.0002});
        const auto rsp = rt.match_response_with_capture(1, 13, body, cap);
        TEST_EXPECT(!rsp.has_value());
    }

    // List structure match
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::list({
            secs::ii::Item::u1({1}),
            secs::ii::Item::u1({2}),
        });
        const auto rsp = rt.match_response_with_capture(1, 15, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_list");
    }
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::list({
            secs::ii::Item::u1({1}),
            secs::ii::Item::u1({2}),
            secs::ii::Item::u1({3}),
        });
        const auto rsp = rt.match_response_with_capture(1, 15, body, cap);
        TEST_EXPECT(!rsp.has_value());
    }

    // List capture (captures the whole list item)
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::list({
            secs::ii::Item::u1({1}),
            secs::ii::Item::u1({2}),
        });
        const auto rsp = rt.match_response_with_capture(1, 17, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_cap_list");

        const auto *v = cap.get("CAP_L");
        TEST_EXPECT(v != nullptr);
        const auto *l = v->get_if<secs::ii::List>();
        TEST_EXPECT(l != nullptr);
        TEST_EXPECT_EQ(l->size(), 2u);
    }
}

void test_match_response_with_capture_pattern_capture_and_mismatch_branches() {
    secs::sml::Runtime rt;
    const char *source = R"(
if (S1F21 <I1 $CAP>) r_i1.
if (S1F22 <I2 $CAP>) r_i2.
if (S1F23 <I4 $CAP>) r_i4.
if (S1F24 <I8 $CAP>) r_i8.
if (S1F25 <U1 $CAP>) r_u1.
if (S1F26 <U2 $CAP>) r_u2.
if (S1F27 <U4 $CAP>) r_u4.
if (S1F28 <U8 $CAP>) r_u8.
if (S1F29 <B $CAP>) r_bin.
if (S1F30 <Boolean $CAP>) r_bool.
if (S1F31 <F4 $CAP>) r_f4.
if (S1F32 <F8 $CAP>) r_f8.
)";

    auto ec = rt.load(source);
    TEST_EXPECT_OK(ec);

    // 逐类型覆盖：capture 分支 + 类型不匹配分支。
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 21, secs::ii::Item::i1(std::vector<std::int8_t>{-1}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_i1");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::I1>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 22, secs::ii::Item::i2(std::vector<std::int16_t>{-2}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_i2");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::I2>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 23, secs::ii::Item::i4(std::vector<std::int32_t>{-3}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_i4");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::I4>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 24, secs::ii::Item::i8(std::vector<std::int64_t>{-4}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_i8");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::I8>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 25, secs::ii::Item::u1(std::vector<std::uint8_t>{1}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_u1");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::U1>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 26, secs::ii::Item::u2(std::vector<std::uint16_t>{2}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_u2");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::U2>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 27, secs::ii::Item::u4(std::vector<std::uint32_t>{3}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_u4");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::U4>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 28, secs::ii::Item::u8(std::vector<std::uint64_t>{4}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_u8");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::U8>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto body = secs::ii::Item::binary(
            std::vector<secs::ii::byte>{static_cast<secs::ii::byte>(1)});
        const auto rsp = rt.match_response_with_capture(1, 29, body, cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_bin");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::Binary>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 30, secs::ii::Item::boolean(std::vector<bool>{true}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_bool");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::Boolean>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 31, secs::ii::Item::f4(std::vector<float>{1.5f}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_f4");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::F4>() != nullptr);
    }
    {
        secs::sml::RenderContext cap;
        const auto rsp = rt.match_response_with_capture(
            1, 32, secs::ii::Item::f8(std::vector<double>{2.5}), cap);
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, "r_f8");
        TEST_EXPECT(cap.get("CAP")->get_if<secs::ii::F8>() != nullptr);
    }

    // 类型不匹配：对各类 pattern 传入 ASCII，让 match_pattern() 走 !v 分支。
    {
        for (std::uint8_t f = 21; f <= 32; ++f) {
            secs::sml::RenderContext cap;
            const auto rsp = rt.match_response_with_capture(
                1, f, secs::ii::Item::ascii("x"), cap);
            TEST_EXPECT(!rsp.has_value());
        }
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
    test_match_response_deep_path_indexing_selection();
    test_match_response_deep_path_indexing_not_a_list();
    test_match_response_deep_path_indexing_out_of_bounds();
    test_match_response_with_capture_pattern();
    test_match_response_with_capture_pattern_various_types();
    test_match_response_with_capture_pattern_capture_and_mismatch_branches();
    return secs::tests::run_and_report();
}
