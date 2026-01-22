/**
 * @file test_sml_fuzz.cpp
 * @brief SML：确定性 fuzz + 差分（parse_sml vs Runtime::load）
 */

#include "secs/sml/runtime.hpp"

#include "test_main.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

struct Lcg final {
    std::uint32_t x{0x87654321u};

    [[nodiscard]] std::uint32_t next_u32() noexcept {
        x = x * 1664525u + 1013904223u;
        return x;
    }
};

[[nodiscard]] char random_sml_char(std::uint32_t v) noexcept {
    // 偏向 SML 常见字符，覆盖 lexer/parser 的主分支。
    static constexpr char kAlphabet[] =
        " \n\t"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "<>.:;,_-+*/()[]=!$"
        "\"'\\";

    constexpr std::size_t n = sizeof(kAlphabet) - 1;
    return kAlphabet[v % n];
}

void test_parse_sml_deterministic_fuzz_does_not_crash() {
    Lcg rng{};

    for (std::size_t i = 0; i < 20000u; ++i) {
        const std::size_t n = static_cast<std::size_t>(rng.next_u32() % 512u);
        std::string s;
        s.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            s[j] = random_sml_char(rng.next_u32());
        }

        const std::string_view sv{s.data(), s.size()};
        auto parsed = secs::sml::parse_sml(sv);

        // 差分：parse_sml 成功时，Runtime::load 也应成功（内部同样走 parse_sml）。
        if (!parsed.ec) {
            secs::sml::Runtime rt;
            const auto load_ec = rt.load(sv);
            TEST_EXPECT_OK(load_ec);
        }
    }
}

} // namespace

int main() {
    test_parse_sml_deterministic_fuzz_does_not_crash();
    return ::secs::tests::run_and_report();
}

