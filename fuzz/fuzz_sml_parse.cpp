#include "secs/sml/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    if (!data) {
        return 0;
    }

    // 防止极端输入导致过长解析时间。
    if (size > 64 * 1024) {
        return 0;
    }

    const auto sv = std::string_view{reinterpret_cast<const char *>(data), size};

    (void)secs::sml::parse_sml(sv);

    // 差分：parse_sml 成功时，Runtime::load 不应失败/崩溃。
    secs::sml::Runtime rt;
    (void)rt.load(sv);

    return 0;
}

