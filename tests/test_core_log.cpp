#include "secs/core/log.hpp"

#include "test_main.hpp"

namespace {

using secs::core::LogLevel;
using secs::core::log_level;
using secs::core::set_log_level;

void test_log_level_roundtrip() {
    set_log_level(LogLevel::trace);
    TEST_EXPECT_EQ(log_level(), LogLevel::trace);

    set_log_level(LogLevel::debug);
    TEST_EXPECT_EQ(log_level(), LogLevel::debug);

    set_log_level(LogLevel::info);
    TEST_EXPECT_EQ(log_level(), LogLevel::info);

    set_log_level(LogLevel::warn);
    TEST_EXPECT_EQ(log_level(), LogLevel::warn);

    set_log_level(LogLevel::error);
    TEST_EXPECT_EQ(log_level(), LogLevel::error);

    set_log_level(LogLevel::critical);
    TEST_EXPECT_EQ(log_level(), LogLevel::critical);

    set_log_level(LogLevel::off);
    TEST_EXPECT_EQ(log_level(), LogLevel::off);
}

} // namespace

int main() {
    test_log_level_roundtrip();
    return ::secs::tests::run_and_report();
}
