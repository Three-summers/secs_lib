#include "secs/ii/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    using secs::core::byte;
    using secs::core::bytes_view;
    using secs::ii::DecodeLimits;
    using secs::ii::Item;

    if (!data) {
        return 0;
    }

    DecodeLimits limits{};
    limits.max_depth = 32;
    limits.max_list_items = 1024;
    limits.max_payload_bytes = 4096;
    limits.max_total_items = 4096;
    limits.max_total_bytes = 16 * 1024;

    Item out = Item::list({});
    std::size_t consumed = 0;
    const auto ec = secs::ii::decode_one(
        bytes_view{reinterpret_cast<const byte *>(data), size},
        out,
        consumed,
        limits);

    if (!ec) {
        // 差分：decode -> encode -> decode（保证不会崩溃，且可在有 sanitizer 时捕捉 UB/OOB）。
        std::vector<byte> encoded;
        (void)secs::ii::encode(out, encoded);

        Item out2 = Item::list({});
        std::size_t consumed2 = 0;
        (void)secs::ii::decode_one(bytes_view{encoded.data(), encoded.size()},
                                   out2,
                                   consumed2,
                                   limits);
    }

    return 0;
}

