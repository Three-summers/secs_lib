#include "secs/sml/runtime.hpp"

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

} // namespace

int main() {
    test_sf_index_named_first_wins();
    test_sf_index_anonymous_overrides_named();
    return secs::tests::run_and_report();
}

