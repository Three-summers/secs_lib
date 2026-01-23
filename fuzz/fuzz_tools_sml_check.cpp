#include "secs/sml/runtime.hpp"
#include "secs/tools/sml_check.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    if (!data || size == 0) {
        return 0;
    }
    // SML parser 自身已有 fuzz；这里主要覆盖语义检查逻辑，避免输入过大拖慢迭代。
    if (size > (1u << 16)) {
        return 0;
    }

    const std::string_view src(reinterpret_cast<const char *>(data), size);
    auto parsed = secs::sml::parse_sml(src);
    if (parsed.ec) {
        return 0;
    }

    std::vector<secs::tools::SmlCheckDiagnostic> diags;
    diags.reserve(16);
    secs::tools::check_undefined_message_refs(parsed.document, diags);
    secs::tools::check_variable_type_consistency(parsed.document, diags);
    return 0;
}

