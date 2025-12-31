#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <string_view>

namespace {

using secs::core::errc;
using secs::core::make_error_code;

void test_error_category_and_messages() {
  auto ec = make_error_code(errc::timeout);
  TEST_EXPECT(ec.category().name() != nullptr);
  TEST_EXPECT_EQ(std::string_view(ec.category().name()), "secs.core");
  TEST_EXPECT(!ec.message().empty());

  TEST_EXPECT_EQ(make_error_code(errc::cancelled), make_error_code(errc::cancelled));
}

void test_all_error_codes() {
  auto ok = make_error_code(errc::ok);
  TEST_EXPECT_EQ(ok.message(), "ok");

  auto timeout = make_error_code(errc::timeout);
  TEST_EXPECT_EQ(timeout.message(), "timeout");

  auto cancelled = make_error_code(errc::cancelled);
  TEST_EXPECT_EQ(cancelled.message(), "cancelled");

  auto overflow = make_error_code(errc::buffer_overflow);
  TEST_EXPECT_EQ(overflow.message(), "buffer overflow");

  auto invalid = make_error_code(errc::invalid_argument);
  TEST_EXPECT_EQ(invalid.message(), "invalid argument");
}

void test_unknown_error_code() {
  std::error_code ec(9999, secs::core::error_category());
  TEST_EXPECT_EQ(ec.message(), "unknown secs.core error");
}

}  // namespace

int main() {
  test_error_category_and_messages();
  test_all_error_codes();
  test_unknown_error_code();
  return ::secs::tests::run_and_report();
}
