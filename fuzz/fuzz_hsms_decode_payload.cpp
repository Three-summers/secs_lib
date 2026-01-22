#include "secs/hsms/message.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    using secs::core::byte;
    using secs::core::bytes_view;

    if (!data) {
        return 0;
    }

    // 防止单次输入触发过大分配（HSMS 最大 payload 16MB）；fuzz 主要追求路径覆盖。
    if (size > 64 * 1024) {
        return 0;
    }

    const auto in = bytes_view{reinterpret_cast<const byte *>(data), size};

    secs::hsms::Message msg{};
    (void)secs::hsms::decode_payload(in, msg);

    std::size_t consumed = 0;
    (void)secs::hsms::decode_frame(in, msg, consumed);

    // 差分：decode_payload 成功时，re-encode + decode_frame 不应崩溃。
    std::vector<byte> frame;
    if (!secs::hsms::encode_frame(msg, frame)) {
        secs::hsms::Message out{};
        std::size_t consumed2 = 0;
        (void)secs::hsms::decode_frame(
            bytes_view{frame.data(), frame.size()}, out, consumed2);
    }

    return 0;
}

