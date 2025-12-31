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
using secs::ii::Item;
using secs::ii::List;
using secs::ii::ASCII;
using secs::ii::U2;
using secs::ii::F4;

// ============================================================================
// Lexer 测试
// ============================================================================

void test_lexer_basic_tokens() {
  Lexer lexer("S1F1 W <L>.");
  auto result = lexer.tokenize();

  TEST_EXPECT(!result.ec);
  TEST_EXPECT_EQ(result.tokens.size(), 7u);  // S1F1, W, <, L, >, ., EOF

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
  TEST_EXPECT_EQ(result.tokens.size(), 5u);  // <, A, "Hello World", >, EOF

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

  for (const auto& tok : result.tokens) {
    if (tok.is(TokenType::Integer) && tok.value == "123") found_123 = true;
    if (tok.is(TokenType::Integer) && tok.value == "0x1F") found_hex = true;
    if (tok.is(TokenType::Float) && tok.value == "0.5567") found_float = true;
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
  for (const auto& tok : result.tokens) {
    if (tok.is(TokenType::Identifier)) ++identifier_count;
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

  const auto& msg = result.document.messages[0];
  TEST_EXPECT(msg.name.empty());
  TEST_EXPECT_EQ(msg.stream, 1u);
  TEST_EXPECT_EQ(msg.function, 1u);
  TEST_EXPECT(msg.w_bit);
}

void test_parser_named_message() {
  auto result = parse_sml("s1f1_1: S1F1 W <L>.");

  TEST_EXPECT(!result.ec);
  TEST_EXPECT_EQ(result.document.messages.size(), 1u);

  const auto& msg = result.document.messages[0];
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

  const auto& msg = result.document.messages[0];
  auto* list = msg.item.get_if<List>();
  TEST_EXPECT(list != nullptr);
  TEST_EXPECT_EQ(list->size(), 2u);

  // 第一个元素是嵌套 List
  auto* inner = (*list)[0].get_if<List>();
  TEST_EXPECT(inner != nullptr);
  TEST_EXPECT_EQ(inner->size(), 2u);

  // 检查 ASCII
  auto* ascii = (*inner)[0].get_if<ASCII>();
  TEST_EXPECT(ascii != nullptr);
  TEST_EXPECT_EQ(ascii->value, std::string("Hello"));

  // 检查 U2
  auto* u2 = (*inner)[1].get_if<U2>();
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

  const auto& rule = result.document.conditions[0];
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

  const auto& rule = result.document.conditions[0];
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

  const auto& timer = result.document.timers[0];
  TEST_EXPECT_EQ(timer.interval_seconds, 5u);
  TEST_EXPECT_EQ(timer.message_name, std::string("s1f1_1"));
}

void test_parser_quoted_sf() {
  auto result = parse_sml("StatusTank1: 'S1F3' W <L <U2 3001>>.");

  TEST_EXPECT(!result.ec);
  TEST_EXPECT_EQ(result.document.messages.size(), 1u);

  const auto& msg = result.document.messages[0];
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
  runtime.load(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L <A "Hello">>.
  )");

  const auto* msg = runtime.get_message("s1f1");
  TEST_EXPECT(msg != nullptr);
  TEST_EXPECT_EQ(msg->stream, 1u);
  TEST_EXPECT_EQ(msg->function, 1u);

  const auto* msg2 = runtime.get_message("nonexistent");
  TEST_EXPECT(msg2 == nullptr);
}

void test_runtime_match_response() {
  Runtime runtime;
  runtime.load(R"(
    s1f1: S1F1 W <L>.
    s1f2: S1F2 <L>.
    if (s1f1) s1f2.
  )");

  auto response = runtime.match_response(1, 1, Item::list({}));
  TEST_EXPECT(response.has_value());
  TEST_EXPECT_EQ(*response, std::string("s1f2"));

  // 不匹配的消息
  auto no_response = runtime.match_response(2, 1, Item::list({}));
  TEST_EXPECT(!no_response.has_value());
}

void test_runtime_timers() {
  Runtime runtime;
  runtime.load(R"(
    s1f1: S1F1 W <L>.
    every 5 send s1f1.
    every 10 send s1f1.
  )");

  TEST_EXPECT_EQ(runtime.timers().size(), 2u);
  TEST_EXPECT_EQ(runtime.timers()[0].interval_seconds, 5u);
  TEST_EXPECT_EQ(runtime.timers()[1].interval_seconds, 10u);
}

// ============================================================================
// 集成测试 - 解析示例 SML 片段
// ============================================================================

void test_parse_sample_sml_fragment() {
  const char* sml = R"(
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
  const auto* s1f1_1 = result.document.find_message("s1f1_1");
  TEST_EXPECT(s1f1_1 != nullptr);
  TEST_EXPECT_EQ(s1f1_1->stream, 1u);
  TEST_EXPECT_EQ(s1f1_1->function, 1u);
  TEST_EXPECT(s1f1_1->w_bit);

  // 验证 StatusTank1
  const auto* tank = result.document.find_message("StatusTank1");
  TEST_EXPECT(tank != nullptr);
  TEST_EXPECT_EQ(tank->stream, 1u);
  TEST_EXPECT_EQ(tank->function, 3u);
}

}  // namespace

int main() {
  // Lexer 测试
  test_lexer_basic_tokens();
  test_lexer_string();
  test_lexer_numbers();
  test_lexer_comments();
  test_lexer_keywords();

  // Parser 测试
  test_parser_simple_message();
  test_parser_named_message();
  test_parser_nested_list();
  test_parser_if_rule();
  test_parser_if_rule_with_condition();
  test_parser_every_rule();
  test_parser_quoted_sf();

  // Runtime 测试
  test_runtime_load();
  test_runtime_get_message();
  test_runtime_match_response();
  test_runtime_timers();

  // 集成测试
  test_parse_sample_sml_fragment();

  return secs::tests::run_and_report();
}
