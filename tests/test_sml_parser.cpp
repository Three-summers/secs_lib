/**
 * @file test_sml_parser.cpp
 * @brief SML 解析器单元测试
 */

#include "secs/sml/lexer.hpp"
#include "secs/sml/parser.hpp"
#include "secs/sml/runtime.hpp"

#include "test_main.hpp"

#include <string_view>

namespace {

using namespace secs::sml;
using secs::ii::ASCII;
using secs::ii::F4;
using secs::ii::Item;
using secs::ii::List;
using secs::ii::U2;

// ============================================================================
// Lexer 测试
// ============================================================================

void test_lexer_basic_tokens() {
    Lexer lexer("S1F1 W <L>.");
    auto result = lexer.tokenize();

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.tokens.size(),
                   7u); // 期望 token：S1F1, W, <, L, >, ., EOF

    TEST_EXPECT(result.tokens[0].is(TokenType::Identifier));
    TEST_EXPECT_EQ(result.tokens[0].value, std::string("S1F1"));

    TEST_EXPECT(result.tokens[1].is(TokenType::KwW));
    TEST_EXPECT(result.tokens[2].is(TokenType::LAngle));
    TEST_EXPECT(result.tokens[3].is(TokenType::KwL));
    TEST_EXPECT(result.tokens[4].is(TokenType::RAngle));
    TEST_EXPECT(result.tokens[5].is(TokenType::Dot));
    TEST_EXPECT(result.tokens[6].is(TokenType::Eof));
}

void test_lexer_string() {
    Lexer lexer(R"(<A "Hello World">)");
    auto result = lexer.tokenize();

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.tokens.size(),
                   5u); // 期望 token：<, A, \"Hello World\", >, EOF

    TEST_EXPECT(result.tokens[2].is(TokenType::String));
    TEST_EXPECT_EQ(result.tokens[2].value, std::string("Hello World"));
}

void test_lexer_numbers() {
    Lexer lexer("<U2 123 0x1F> <F4 0.5567>");
    auto result = lexer.tokenize();

    TEST_EXPECT(!result.ec);

    // 找到数字类 token（词法单元）
    bool found_123 = false;
    bool found_hex = false;
    bool found_float = false;

    for (const auto &tok : result.tokens) {
        if (tok.is(TokenType::Integer) && tok.value == "123")
            found_123 = true;
        if (tok.is(TokenType::Integer) && tok.value == "0x1F")
            found_hex = true;
        if (tok.is(TokenType::Float) && tok.value == "0.5567")
            found_float = true;
    }

    TEST_EXPECT(found_123);
    TEST_EXPECT(found_hex);
    TEST_EXPECT(found_float);
}

void test_lexer_comments() {
    Lexer lexer("/* comment */ S1F1. // line comment\nS1F2.");
    auto result = lexer.tokenize();

    TEST_EXPECT(!result.ec);

    // 应该有 S1F1, ., S1F2, ., EOF
    int identifier_count = 0;
    for (const auto &tok : result.tokens) {
        if (tok.is(TokenType::Identifier))
            ++identifier_count;
    }
    TEST_EXPECT_EQ(identifier_count, 2);
}

void test_lexer_keywords() {
    Lexer lexer("if every send W L A B Boolean U1 U2 U4 U8 I1 I2 I4 I8 F4 F8");
    auto result = lexer.tokenize();

    TEST_EXPECT(!result.ec);

    TEST_EXPECT(result.tokens[0].is(TokenType::KwIf));
    TEST_EXPECT(result.tokens[1].is(TokenType::KwEvery));
    TEST_EXPECT(result.tokens[2].is(TokenType::KwSend));
    TEST_EXPECT(result.tokens[3].is(TokenType::KwW));
    TEST_EXPECT(result.tokens[4].is(TokenType::KwL));
}

// ============================================================================
// Parser 测试
// ============================================================================

void test_parser_simple_message() {
    auto result = parse_sml("S1F1 W <L>.");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 1u);

    const auto &msg = result.document.messages[0];
    TEST_EXPECT(msg.name.empty());
    TEST_EXPECT_EQ(msg.stream, 1u);
    TEST_EXPECT_EQ(msg.function, 1u);
    TEST_EXPECT(msg.w_bit);
}

void test_parser_named_message() {
    auto result = parse_sml("s1f1_1: S1F1 W <L>.");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 1u);

    const auto &msg = result.document.messages[0];
    TEST_EXPECT_EQ(msg.name, std::string("s1f1_1"));
    TEST_EXPECT_EQ(msg.stream, 1u);
    TEST_EXPECT_EQ(msg.function, 1u);
}

void test_parser_nested_list() {
    auto result = parse_sml(R"(
    S1F1 W
    <L
      <L
        <A "Hello">
        <U2 123>
      >
      <B 0x00 0x01>
    >.
  )");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 1u);

    const auto &msg = result.document.messages[0];
    auto *list = msg.item.get_if<List>();
    TEST_EXPECT(list != nullptr);
    TEST_EXPECT_EQ(list->size(), 2u);

    // 第一个元素是嵌套 List
    auto *inner = (*list)[0].get_if<List>();
    TEST_EXPECT(inner != nullptr);
    TEST_EXPECT_EQ(inner->size(), 2u);

    // 检查 ASCII
    auto *ascii = (*inner)[0].get_if<ASCII>();
    TEST_EXPECT(ascii != nullptr);
    TEST_EXPECT_EQ(ascii->value, std::string("Hello"));

    // 检查 U2
    auto *u2 = (*inner)[1].get_if<U2>();
    TEST_EXPECT(u2 != nullptr);
    TEST_EXPECT_EQ(u2->values.size(), 1u);
    TEST_EXPECT_EQ(u2->values[0], 123u);
}

void test_parser_if_rule() {
    auto result = parse_sml(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L>.
    if (s1f1) s1f2.
  )");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 2u);
    TEST_EXPECT_EQ(result.document.conditions.size(), 1u);

    const auto &rule = result.document.conditions[0];
    TEST_EXPECT_EQ(rule.condition.message_name, std::string("s1f1"));
    TEST_EXPECT_EQ(rule.response_name, std::string("s1f2"));
}

void test_parser_if_rule_with_condition() {
    auto result = parse_sml(R"(
    s1f1: S1F1 W <L <F4 0.5567>>.
    s1f2_1: S1F2 <L>.
    if (s1f1 (1)==<F4 0.5567>) s1f2_1.
  )");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.conditions.size(), 1u);

    const auto &rule = result.document.conditions[0];
    TEST_EXPECT(rule.condition.index.has_value());
    TEST_EXPECT_EQ(*rule.condition.index, 1u);
    TEST_EXPECT(rule.condition.expected.has_value());
}

void test_parser_every_rule() {
    auto result = parse_sml(R"(
    s1f1_1: S1F1 W <L>.
    every 5 send s1f1_1.
  )");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.timers.size(), 1u);

    const auto &timer = result.document.timers[0];
    TEST_EXPECT_EQ(timer.interval_seconds, 5u);
    TEST_EXPECT_EQ(timer.message_name, std::string("s1f1_1"));
}

void test_parser_quoted_sf() {
    auto result = parse_sml("StatusTank1: 'S1F3' W <L <U2 3001>>.");

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 1u);

    const auto &msg = result.document.messages[0];
    TEST_EXPECT_EQ(msg.name, std::string("StatusTank1"));
    TEST_EXPECT_EQ(msg.stream, 1u);
    TEST_EXPECT_EQ(msg.function, 3u);
}

// ============================================================================
// Runtime 测试
// ============================================================================

void test_runtime_load() {
    Runtime runtime;
    auto ec = runtime.load(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L <A "Hello">>.
  )");

    TEST_EXPECT(!ec);
    TEST_EXPECT(runtime.loaded());
    TEST_EXPECT_EQ(runtime.messages().size(), 2u);
}

void test_runtime_get_message() {
    Runtime runtime;
    auto ec = runtime.load(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L <A "Hello">>.
  )");
    TEST_EXPECT_OK(ec);

    const auto *msg = runtime.get_message("s1f1");
    TEST_EXPECT(msg != nullptr);
    TEST_EXPECT_EQ(msg->stream, 1u);
    TEST_EXPECT_EQ(msg->function, 1u);

    const auto *msg2 = runtime.get_message("nonexistent");
    TEST_EXPECT(msg2 == nullptr);
}

void test_runtime_match_response() {
    Runtime runtime;
    auto ec = runtime.load(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L>.
    if (s1f1) s1f2.
  )");
    TEST_EXPECT_OK(ec);

    auto response = runtime.match_response(1, 1, Item::list({}));
    TEST_EXPECT(response.has_value());
    TEST_EXPECT_EQ(*response, std::string("s1f2"));

    // 不匹配的消息
    auto no_response = runtime.match_response(2, 1, Item::list({}));
    TEST_EXPECT(!no_response.has_value());
}

void test_runtime_timers() {
    Runtime runtime;
    auto ec = runtime.load(R"(
    s1f1: S1F1 W <L>.
    every 5 send s1f1.
    every 10 send s1f1.
  )");
    TEST_EXPECT_OK(ec);

    TEST_EXPECT_EQ(runtime.timers().size(), 2u);
    TEST_EXPECT_EQ(runtime.timers()[0].interval_seconds, 5u);
    TEST_EXPECT_EQ(runtime.timers()[1].interval_seconds, 10u);
}

// ============================================================================
// Lexer/Parser 错误与边界测试（恶意输入）
// ============================================================================

void test_lexer_error_category_messages() {
    using std::string_view;

    const auto &cat = lexer_error_category();
    TEST_EXPECT_EQ(string_view(cat.name()), "sml.lexer");

    TEST_EXPECT_EQ(make_error_code(lexer_errc::ok).message(), "success");
    TEST_EXPECT_EQ(make_error_code(lexer_errc::unterminated_string).message(),
                   "unterminated string literal");
    TEST_EXPECT_EQ(make_error_code(lexer_errc::unterminated_comment).message(),
                   "unterminated block comment");
    TEST_EXPECT_EQ(make_error_code(lexer_errc::invalid_character).message(),
                   "invalid character");
    TEST_EXPECT_EQ(make_error_code(lexer_errc::invalid_hex_literal).message(),
                   "invalid hexadecimal literal");
}

void test_parser_error_category_messages() {
    using std::string_view;

    const auto &cat = parser_error_category();
    TEST_EXPECT_EQ(string_view(cat.name()), "sml.parser");

    TEST_EXPECT_EQ(make_error_code(parser_errc::ok).message(), "success");
    TEST_EXPECT_EQ(make_error_code(parser_errc::unexpected_token).message(),
                   "unexpected token");
    TEST_EXPECT_EQ(make_error_code(parser_errc::expected_item).message(),
                   "expected item");
    TEST_EXPECT_EQ(make_error_code(parser_errc::expected_identifier).message(),
                   "expected identifier");
    TEST_EXPECT_EQ(make_error_code(parser_errc::expected_number).message(),
                   "expected number");
    TEST_EXPECT_EQ(
        make_error_code(parser_errc::invalid_stream_function).message(),
        "invalid stream/function format");
    TEST_EXPECT_EQ(make_error_code(parser_errc::unclosed_item).message(),
                   "unclosed item");
    TEST_EXPECT_EQ(make_error_code(parser_errc::invalid_condition).message(),
                   "invalid condition");
}

void test_lexer_unterminated_block_comment_is_error() {
    Lexer lexer("/* not closed");
    auto result = lexer.tokenize();
    TEST_EXPECT_EQ(result.ec,
                   make_error_code(lexer_errc::unterminated_comment));
    TEST_EXPECT_EQ(result.error_line, 1u);
    TEST_EXPECT_EQ(result.error_column, 1u);
}

void test_lexer_unterminated_string_is_error() {
    {
        Lexer lexer("\"abc");
        auto result = lexer.tokenize();
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(lexer_errc::unterminated_string));
        TEST_EXPECT_EQ(result.error_line, 1u);
        TEST_EXPECT_EQ(result.error_column, 1u);
    }
    {
        Lexer lexer("\"abc\n");
        auto result = lexer.tokenize();
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(lexer_errc::unterminated_string));
        TEST_EXPECT_EQ(result.error_line, 1u);
        TEST_EXPECT_EQ(result.error_column, 1u);
    }
}

void test_lexer_invalid_hex_literal_is_error() {
    Lexer lexer("0x");
    auto result = lexer.tokenize();
    TEST_EXPECT_EQ(result.ec, make_error_code(lexer_errc::invalid_hex_literal));
    TEST_EXPECT_EQ(result.error_line, 1u);
    TEST_EXPECT_EQ(result.error_column, 1u);
}

void test_lexer_unexpected_equal_is_error() {
    Lexer lexer("=");
    auto result = lexer.tokenize();
    TEST_EXPECT_EQ(result.ec, make_error_code(lexer_errc::invalid_character));
    TEST_EXPECT_EQ(result.error_line, 1u);
    TEST_EXPECT_EQ(result.error_column, 1u);
}

void test_lexer_string_escapes() {
    Lexer lexer(R"("a\n\t\r\\\"\'\q")");
    auto result = lexer.tokenize();
    TEST_EXPECT_OK(result.ec);
    TEST_EXPECT(result.tokens.size() >= 2u);
    TEST_EXPECT(result.tokens[0].is(TokenType::String));

    const auto &s = result.tokens[0].value;
    TEST_EXPECT(s.size() == 8u);
    TEST_EXPECT_EQ(s[0], 'a');
    TEST_EXPECT_EQ(s[1], '\n');
    TEST_EXPECT_EQ(s[2], '\t');
    TEST_EXPECT_EQ(s[3], '\r');
    TEST_EXPECT_EQ(s[4], '\\');
    TEST_EXPECT_EQ(s[5], '"');
    TEST_EXPECT_EQ(s[6], '\'');
    TEST_EXPECT_EQ(s[7], 'q');
}

void test_parse_sml_propagates_lexer_error() {
    auto result = parse_sml("@");
    TEST_EXPECT_EQ(result.ec, make_error_code(lexer_errc::invalid_character));
    TEST_EXPECT_EQ(result.error_line, 1u);
    TEST_EXPECT_EQ(result.error_column, 1u);
    TEST_EXPECT(!result.error_message.empty());
}

void test_parser_unclosed_item_is_error() {
    auto result = parse_sml(R"(S1F1 <A "x".)");
    TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::unclosed_item));
    TEST_EXPECT(result.error_line > 0u);
    TEST_EXPECT(result.error_column > 0u);
}

void test_parser_binary_out_of_range_is_error() {
    auto result = parse_sml("S1F1 <B 0x1FF>.");
    TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::expected_number));
}

void test_parser_every_interval_out_of_range_is_error() {
    auto result = parse_sml("every -1 send s1f1.");
    TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::expected_number));
}

void test_parser_if_empty_condition_is_error() {
    auto result = parse_sml("if () s1f2.");
    TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::invalid_condition));
}

// ============================================================================
// Parser 深入覆盖 - 多类型 Item / 更多语法分支
// ============================================================================

void test_parse_sml_exercises_all_item_types() {
    const char *sml = R"(
    /* 覆盖尽可能多的 Item 类型与语法分支 */

    all: S1F1 W
    <L[0]
      <A "hello">
      <A>
      <B 0x00 0x01 2 0xFF>
      <Boolean 0 1 -1>
      <U1 0 255>
      <U2 0 65535>
      <U4 0 4294967295>
      <U8 0 18446744073709551615>
      <I1 -128 127>
      <I2 -32768 32767>
      <I4 -2147483648 2147483647>
      <I8 -9223372036854775808 9223372036854775807>
      <F4 1 2.5 1.0e-3>
      <F8 1 2.5 1.0e-3>
      <L[0]>
    >.

    rsp: S1F2 <L>.
    if (all(1)==<A "hello">) rsp.
    if (S1F1) rsp.
    every 1 send all.
  )";

    auto result = parse_sml(sml);
    TEST_EXPECT_OK(result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 2u);
    TEST_EXPECT_EQ(result.document.conditions.size(), 2u);
    TEST_EXPECT_EQ(result.document.timers.size(), 1u);

    Runtime rt;
    auto ec = rt.load(sml);
    TEST_EXPECT_OK(ec);
    auto rsp = rt.match_response(1, 1, Item::list({Item::ascii("hello")}));
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, std::string("rsp"));
}

void test_parser_more_syntax_error_branches() {
    // 1) 非法起始 Token：触发 “expected message definition”
    {
        auto result = parse_sml(".");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // 2) 冒号后缺少 SxFy：触发 “expected stream/function after ':'”
    {
        auto result = parse_sml("name: <L>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // 3) SxFy 格式错误：触发 invalid_stream_function
    {
        auto result = parse_sml("name: BAD <L>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::invalid_stream_function));
    }

    // 4) 消息定义缺少 '.' 结尾
    {
        auto result = parse_sml("S1F1 <L>");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }
}

void test_parser_if_every_error_branches() {
    // if 后缺少 '('
    {
        auto result = parse_sml("if s1f1) rsp.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // 条件后缺少 ')'
    {
        auto result = parse_sml("if (s1f1 rsp.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // 响应名不是标识符
    {
        auto result = parse_sml(R"(if (s1f1) <L>.)");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // if 规则缺少 '.' 结尾
    {
        auto result = parse_sml("if (s1f1) rsp");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // every 后缺少间隔秒数
    {
        auto result = parse_sml("every send s1f1.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // every N 后缺少 send
    {
        auto result = parse_sml("every 1 s1f1.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // every N send 后缺少消息名
    {
        auto result = parse_sml("every 1 send.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // every 规则缺少 '.' 结尾
    {
        auto result = parse_sml("every 1 send s1f1");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }
}

void test_parser_item_and_condition_error_branches() {
    // 条件中 '==' 后面缺少 '<'：应报 expected_item
    {
        auto result = parse_sml("if (s1f1==s1f2) rsp.");
        TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::expected_item));
    }

    // 未知 Item 类型：应报 expected_item
    {
        auto result = parse_sml("S1F1 <Z>.");
        TEST_EXPECT_EQ(result.ec, make_error_code(parser_errc::expected_item));
    }

    // List 的 [n] 大小提示缺少 ']'：应报 unexpected_token
    {
        auto result = parse_sml(R"(S1F1 <L[2 <A "x">>.)");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::unexpected_token));
    }

    // 条件中消息名不是标识符：应报 invalid_condition
    {
        auto result = parse_sml(R"(if (<A "x">) rsp.)");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::invalid_condition));
    }

    // 索引不是数字：应报 expected_number
    {
        auto result = parse_sml(R"(if (s1f1("x")) rsp.)");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // 索引为负数：应报 expected_number（uint 解析失败）
    {
        auto result = parse_sml("if (s1f1(-1)) rsp.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // 索引为 0：应报 invalid_condition（索引必须 >= 1）
    {
        auto result = parse_sml("if (s1f1(0)) rsp.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::invalid_condition));
    }

    // 索引括号未闭合：应报 invalid_condition
    {
        auto result = parse_sml(R"(if (s1f1(1==<A "OK">)) rsp.)");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::invalid_condition));
    }
}

void test_parser_numeric_range_checks_more() {
    // Boolean：超出 int64 的数值应报错
    {
        auto result = parse_sml("S1F1 <Boolean 9223372036854775808>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // U1：256 超范围
    {
        auto result = parse_sml("S1F1 <U1 256>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // U8：uint64_t 溢出
    {
        auto result = parse_sml("S1F1 <U8 18446744073709551616>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }

    // I1：128 超范围
    {
        auto result = parse_sml("S1F1 <I1 128>.");
        TEST_EXPECT_EQ(result.ec,
                       make_error_code(parser_errc::expected_number));
    }
}

// ============================================================================
// Runtime 更完整覆盖（查找、索引、容差匹配）
// ============================================================================

void test_document_find_message_and_find_by_sf() {
    Document doc;
    MessageDef named;
    named.name = "AreYouThere";
    named.stream = 1;
    named.function = 1;
    doc.messages.push_back(named);

    const auto *found = doc.find_message("AreYouThere");
    TEST_EXPECT(found != nullptr);
    TEST_EXPECT_EQ(found->stream, 1u);

    const auto *not_found = doc.find_message("Missing");
    TEST_EXPECT(not_found == nullptr);

    const auto *sf_missing = doc.find_by_sf(1, 1);
    TEST_EXPECT(sf_missing == nullptr);

    MessageDef anon;
    anon.name = "";
    anon.stream = 2;
    anon.function = 3;
    doc.messages.push_back(anon);
    const auto *sf_found = doc.find_by_sf(2, 3);
    TEST_EXPECT(sf_found != nullptr);
}

void test_runtime_get_message_by_sf_paths() {
    {
        Runtime rt;
        auto ec = rt.load(R"(
      S1F1 <L>.
      named: S1F1 <L>.
    )");
        TEST_EXPECT_OK(ec);

        const auto *m = rt.get_message(1, 1);
        TEST_EXPECT(m != nullptr);
    }

    {
        Runtime rt;
        auto ec = rt.load(R"(
      AreYouThere: S1F1 <L>.
    )");
        TEST_EXPECT_OK(ec);

        const auto *m = rt.get_message(1, 1);
        TEST_EXPECT(m != nullptr);
        TEST_EXPECT_EQ(m->name, std::string("AreYouThere"));

        const auto *none = rt.get_message(9, 9);
        TEST_EXPECT(none == nullptr);
    }
}

void test_runtime_condition_index_and_expected_matching() {
    Runtime rt;
    auto ec = rt.load(R"(
    rsp: S1F2 <L>.
    if (S1F1(1)==<A "OK">) rsp.
  )");
    TEST_EXPECT_OK(ec);

    // 1) item 不是 List -> 不匹配
    TEST_EXPECT(!rt.match_response(1, 1, Item::ascii("OK")).has_value());

    // 2) idx 越界 -> 不匹配
    TEST_EXPECT(!rt.match_response(1, 1, Item::list({})).has_value());

    // 3) 元素不相等 -> 不匹配
    TEST_EXPECT(
        !rt.match_response(
               1, 1, Item::list({Item::ascii("BAD"), Item::ascii("OK")}))
             .has_value());

    // 4) 命中：第 1 个元素为 "OK"
    auto rsp = rt.match_response(1, 1, Item::list({Item::ascii("OK")}));
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, std::string("rsp"));
}

void test_runtime_condition_name_lookup_paths() {
    Runtime rt;
    auto ec = rt.load(R"(
    S128F1: S1F1 <L>.
    S1F1x: S1F1 <L>.
    if (S128F1) S1F1x.
  )");
    TEST_EXPECT_OK(ec);

    // message_name="S128F1" 会因 stream
    // 超范围导致按“名称”查找，仍应能命中规则。
    auto rsp = rt.match_response(1, 1, Item::list({}));
    TEST_EXPECT(rsp.has_value());
    TEST_EXPECT_EQ(*rsp, std::string("S1F1x"));
}

void test_runtime_float_tolerance_match() {
    Runtime rt;
    auto ec = rt.load(R"(
    rsp: S1F2 <L>.
    if (S1F1(1)==<F4 1.0>) rsp.
  )");
    TEST_EXPECT_OK(ec);

    // 误差在容差内 -> 命中
    auto ok_rsp = rt.match_response(
        1, 1, Item::list({Item::f4(std::vector<float>{1.00005f})}));
    TEST_EXPECT(ok_rsp.has_value());

    // 误差过大 -> 不命中
    auto bad_rsp = rt.match_response(
        1, 1, Item::list({Item::f4(std::vector<float>{1.1f})}));
    TEST_EXPECT(!bad_rsp.has_value());
}

void test_runtime_parse_sf_failure_branches() {
    Runtime rt;
    auto ec = rt.load(R"(
    rsp: S1F2 <L>.

    A1F1: S1F1 <L>.
    SF1X: S2F1 <L>.
    SxF1: S3F1 <L>.
    S1Fx: S4F1 <L>.

    if (A1F1) rsp.
    if (SF1X) rsp.
    if (SxF1) rsp.
    if (S1Fx) rsp.
  )");
    TEST_EXPECT_OK(ec);

    // 这些 message_name 都满足“看起来像 SxFy，但 parse_sf
    // 会失败”的场景，用来覆盖失败分支后回退到名称查找。
    {
        auto rsp = rt.match_response(1, 1, Item::list({}));
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, std::string("rsp"));
    }
    {
        auto rsp = rt.match_response(2, 1, Item::list({}));
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, std::string("rsp"));
    }
    {
        auto rsp = rt.match_response(3, 1, Item::list({}));
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, std::string("rsp"));
    }
    {
        auto rsp = rt.match_response(4, 1, Item::list({}));
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, std::string("rsp"));
    }
}

void test_runtime_double_tolerance_and_mismatch_paths() {
    Runtime rt;
    auto ec = rt.load(R"(
    f4_bad_type: S1F2 <L>.
    f4_bad_size: S2F2 <L>.
    f8_bad_type: S3F2 <L>.
    f8_bad_size: S4F2 <L>.
    f8_ok: S5F2 <L>.

    if (S1F1(1)==<A "X">) f4_bad_type.
    if (S2F1(1)==<F4 1.0 2.0>) f4_bad_size.
    if (S3F1(1)==<A "X">) f8_bad_type.
    if (S4F1(1)==<F8 1.0 2.0>) f8_bad_size.
    if (S5F1(1)==<F8 1.0>) f8_ok.
  )");
    TEST_EXPECT_OK(ec);

    // F4：期望类型不匹配 -> items_equal(F4) 走 bf4==nullptr 分支
    TEST_EXPECT(!rt.match_response(
                       1, 1, Item::list({Item::f4(std::vector<float>{1.0f})}))
                     .has_value());

    // F4：长度不匹配 -> items_equal(F4) 走 size mismatch 分支
    TEST_EXPECT(!rt.match_response(
                       2, 1, Item::list({Item::f4(std::vector<float>{1.0f})}))
                     .has_value());

    // F8：期望类型不匹配 -> items_equal(F8) 走 bf8==nullptr 分支
    TEST_EXPECT(!rt.match_response(
                       3, 1, Item::list({Item::f8(std::vector<double>{1.0})}))
                     .has_value());

    // F8：长度不匹配 -> items_equal(F8) 走 size mismatch 分支
    TEST_EXPECT(!rt.match_response(
                       4, 1, Item::list({Item::f8(std::vector<double>{1.0})}))
                     .has_value());

    // F8：容差内 -> 命中（覆盖 double_almost_equal + items_equal 返回 true）
    {
        auto rsp = rt.match_response(
            5, 1, Item::list({Item::f8(std::vector<double>{1.00005})}));
        TEST_EXPECT(rsp.has_value());
        TEST_EXPECT_EQ(*rsp, std::string("f8_ok"));
    }

    // F8：误差过大 -> 不命中（覆盖 double_almost_equal 返回 false）
    TEST_EXPECT(!rt.match_response(
                       5, 1, Item::list({Item::f8(std::vector<double>{1.1})}))
                     .has_value());
}

// ============================================================================
// 集成测试 - 解析示例 SML 片段
// ============================================================================

void test_parse_sample_sml_fragment() {
    const char *sml = R"(
    /* 示例 SML 片段 */
    S1F0.

    s1f1_1: S1F1 W
    <L
      <L
        <Boolean 0x01 0x00>
        <F4 0.5567>
        <L
          <F8 0.9>
        >
      >
      <B 0x00 0x05 0x06 0x09 0xff>
    >.

    s1f2_1: S1F2
    <L
      <A "Hi Good Idea!">
      <A "Are you Okay!">
    >.

    StatusTank1: 'S1F3' W
    <L
      <U2 3001>
      <U2 3002>
    >.

    if (s1f1_1 (2)==<F4 0.5567>) s1f2_1.
    every 5 send s1f1_1.
  )";

    auto result = parse_sml(sml);

    TEST_EXPECT(!result.ec);
    TEST_EXPECT_EQ(result.document.messages.size(), 4u);
    TEST_EXPECT_EQ(result.document.conditions.size(), 1u);
    TEST_EXPECT_EQ(result.document.timers.size(), 1u);

    // 验证 s1f1_1
    const auto *s1f1_1 = result.document.find_message("s1f1_1");
    TEST_EXPECT(s1f1_1 != nullptr);
    TEST_EXPECT_EQ(s1f1_1->stream, 1u);
    TEST_EXPECT_EQ(s1f1_1->function, 1u);
    TEST_EXPECT(s1f1_1->w_bit);

    // 验证 StatusTank1
    const auto *tank = result.document.find_message("StatusTank1");
    TEST_EXPECT(tank != nullptr);
    TEST_EXPECT_EQ(tank->stream, 1u);
    TEST_EXPECT_EQ(tank->function, 3u);
}

} // namespace

int main() {
    // Lexer 测试
    test_lexer_basic_tokens();
    test_lexer_string();
    test_lexer_numbers();
    test_lexer_comments();
    test_lexer_keywords();
    test_lexer_error_category_messages();
    test_lexer_unterminated_block_comment_is_error();
    test_lexer_unterminated_string_is_error();
    test_lexer_invalid_hex_literal_is_error();
    test_lexer_unexpected_equal_is_error();
    test_lexer_string_escapes();

    // Parser 测试
    test_parser_simple_message();
    test_parser_named_message();
    test_parser_nested_list();
    test_parser_if_rule();
    test_parser_if_rule_with_condition();
    test_parser_every_rule();
    test_parser_quoted_sf();
    test_parser_error_category_messages();
    test_parse_sml_propagates_lexer_error();
    test_parser_unclosed_item_is_error();
    test_parser_binary_out_of_range_is_error();
    test_parser_every_interval_out_of_range_is_error();
    test_parser_if_empty_condition_is_error();
    test_parse_sml_exercises_all_item_types();
    test_parser_more_syntax_error_branches();
    test_parser_if_every_error_branches();
    test_parser_item_and_condition_error_branches();
    test_parser_numeric_range_checks_more();

    // Runtime 测试
    test_runtime_load();
    test_runtime_get_message();
    test_runtime_match_response();
    test_runtime_timers();
    test_document_find_message_and_find_by_sf();
    test_runtime_get_message_by_sf_paths();
    test_runtime_condition_index_and_expected_matching();
    test_runtime_condition_name_lookup_paths();
    test_runtime_float_tolerance_match();
    test_runtime_parse_sf_failure_branches();
    test_runtime_double_tolerance_and_mismatch_paths();

    // 集成测试
    test_parse_sample_sml_fragment();

    return secs::tests::run_and_report();
}
