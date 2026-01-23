/**
 * @file test_tools_sml_check.cpp
 * @brief tools::SML 语义检查（未定义引用/变量类型一致性）单元测试
 */

#include "secs/sml/runtime.hpp"
#include "secs/tools/sml_check.hpp"

#include "test_main.hpp"

#include <string_view>
#include <vector>

namespace {

using secs::tools::SmlCheckDiagnostic;
using secs::tools::SmlCheckDiagnosticKind;

void test_sml_check_undefined_message_reference_in_condition_response() {
    const std::string_view src = R"(
known_response: S1F2 <L>.
if (S1F1 <L>) unknown_response.
)";

    auto parsed = secs::sml::parse_sml(src);
    TEST_EXPECT_OK(parsed.ec);

    std::vector<SmlCheckDiagnostic> diags;
    secs::tools::check_undefined_message_refs(parsed.document, diags);

    TEST_EXPECT_EQ(diags.size(), 1u);
    TEST_EXPECT_EQ(diags[0].kind,
                   SmlCheckDiagnosticKind::undefined_message_reference);
    TEST_EXPECT(diags[0].span.line != 0);
    TEST_EXPECT(diags[0].span.column != 0);
}

void test_sml_check_variable_type_mismatch_between_template_and_capture() {
    const std::string_view src = R"(
resp: S1F2 <L <U2 DATAID>>.
if (S1F1 <L <A $DATAID>>) resp.
)";

    auto parsed = secs::sml::parse_sml(src);
    TEST_EXPECT_OK(parsed.ec);

    std::vector<SmlCheckDiagnostic> diags;
    secs::tools::check_variable_type_consistency(parsed.document, diags);

    TEST_EXPECT_EQ(diags.size(), 1u);
    TEST_EXPECT_EQ(diags[0].kind, SmlCheckDiagnosticKind::type_mismatch);
    TEST_EXPECT(diags[0].span.line != 0);
    TEST_EXPECT(diags[0].span.column != 0);
}

} // namespace

int main() {
    test_sml_check_undefined_message_reference_in_condition_response();
    test_sml_check_variable_type_mismatch_between_template_and_capture();
    return secs::tests::run_and_report();
}

