#include "secs/tools/recording.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    if (!data || size == 0) {
        return 0;
    }
    // 避免极端大输入导致无意义的巨量分配，保持 fuzz 迭代效率。
    if (size > 4096) {
        return 0;
    }

    const std::string_view line(reinterpret_cast<const char *>(data), size);

    secs::tools::RecordedMessage msg{};
    const auto ec = secs::tools::MessagePlayer::parse_jsonl_line(line, msg);
    if (ec) {
        return 0;
    }

    if (msg.body.size() > 2048) {
        return 0;
    }

    const auto out = secs::tools::MessagePlayer::to_jsonl_line(msg);
    secs::tools::RecordedMessage msg2{};
    (void)secs::tools::MessagePlayer::parse_jsonl_line(out, msg2);
    return 0;
}

