#pragma once

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace secs::tests {

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline void record_failure(
  const char* file,
  int line,
  std::string_view message) {
  ++failure_count();
  std::cerr << file << ":" << line << ": " << message << "\n";
}

inline void expect_true(bool value, const char* expr, const char* file, int line) {
  if (value) {
    return;
  }
  std::ostringstream oss;
  oss << "EXPECT failed: " << expr;
  record_failure(file, line, oss.str());
}

template <class L, class R>
inline void expect_eq(
  const L& lhs,
  const R& rhs,
  const char* lhs_expr,
  const char* rhs_expr,
  const char* file,
  int line) {
  if (lhs == rhs) {
    return;
  }
  std::ostringstream oss;
  oss << "EXPECT_EQ failed: (" << lhs_expr << ") != (" << rhs_expr << ")";
  record_failure(file, line, oss.str());
}

inline void expect_ok(const std::error_code& ec, const char* expr, const char* file, int line) {
  if (!ec) {
    return;
  }
  std::ostringstream oss;
  oss << "EXPECT_OK failed: " << expr << " -> [" << ec.category().name() << "] " << ec.message();
  record_failure(file, line, oss.str());
}

inline int run_and_report() {
  if (failure_count() == 0) {
    return 0;
  }
  std::cerr << "FAILED: " << failure_count() << " assertions\n";
  return 1;
}

}  // namespace secs::tests

#define TEST_EXPECT(expr) ::secs::tests::expect_true((expr), #expr, __FILE__, __LINE__)
#define TEST_EXPECT_EQ(a, b) ::secs::tests::expect_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define TEST_EXPECT_OK(ec) ::secs::tests::expect_ok((ec), #ec, __FILE__, __LINE__)
#define TEST_FAIL(msg) ::secs::tests::record_failure(__FILE__, __LINE__, (msg))
