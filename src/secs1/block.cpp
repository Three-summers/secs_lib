#include "secs/secs1/block.hpp"

#include "secs/core/error.hpp"

#include <algorithm>
#include <string>

namespace secs::secs1 {
namespace {

/*
 * SECS-I Block 编解码说明（对应 include/secs/secs1/block.hpp 的协议描述）：
 *
 * - frame = Length(1B) + Header(10B) + Data(NB) + Checksum(2B)
 * - Checksum 为对 (Header+Data) 的逐字节求和（mod 65536），以大端序写入 2
 * 字节。
 *
 * 该文件实现只做“字节级编解码与校验”，不处理 ENQ/EOT/ACK/NAK 的链路控制。
 */
class secs1_error_category final : public std::error_category {
public:
    const char *name() const noexcept override { return "secs.secs1"; }

    std::string message(int ev) const override {
        switch (static_cast<errc>(ev)) {
        case errc::ok:
            return "ok";
        case errc::invalid_block:
            return "invalid block";
        case errc::checksum_mismatch:
            return "checksum mismatch";
        case errc::device_id_mismatch:
            return "device id mismatch";
        case errc::protocol_error:
            return "protocol error";
        case errc::too_many_retries:
            return "too many retries";
        case errc::block_sequence_error:
            return "block sequence error";
        default:
            return "unknown secs.secs1 error";
        }
    }
};

} // namespace

const std::error_category &error_category() noexcept {
    static secs1_error_category category;
    return category;
}

std::error_code make_error_code(errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

std::uint16_t checksum(secs::core::bytes_view bytes) noexcept {
    // 校验和：逐字节求和后取低 16 位（mod 65536）。
    std::uint32_t sum = 0;
    for (const auto b : bytes) {
        sum += b;
    }
    return static_cast<std::uint16_t>(sum & 0xFFFF);
}

std::error_code encode_block(const Header &header,
                             secs::core::bytes_view data,
                             std::vector<secs::core::byte> &out) {
    if (header.device_id > 0x7FFF) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    if (header.block_number > 0x00FF) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    if (data.size() > kMaxBlockDataSize) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    const auto length = kHeaderSize + data.size();
    if (length < kHeaderSize || length > kMaxBlockLength) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    out.clear();
    out.reserve(1 + length + 2);
    out.push_back(static_cast<secs::core::byte>(length));

    // 头部字段的位打包规则：
    // - DeviceID / BlockNumber 的最高位与 reverse_bit/end_bit
    // 共用一个字节（高位为标志，低 7 位为高位数据）
    const auto dev_hi =
        static_cast<secs::core::byte>((header.device_id >> 8) & 0x7F);
    const auto dev_lo = static_cast<secs::core::byte>(header.device_id & 0xFF);

    out.push_back(static_cast<secs::core::byte>(
        (header.reverse_bit ? 0x80 : 0x00) | dev_hi));
    out.push_back(dev_lo);

    out.push_back(static_cast<secs::core::byte>(
        (header.wait_bit ? 0x80 : 0x00) | (header.stream & 0x7F)));
    out.push_back(static_cast<secs::core::byte>(header.function));

    // 对齐 c_dump：BlockNumber 只占 1 字节（Byte6），Byte5 的低 7 位保留为 0。
    out.push_back(static_cast<secs::core::byte>(header.end_bit ? 0x80 : 0x00));
    out.push_back(
        static_cast<secs::core::byte>(static_cast<std::uint8_t>(
            header.block_number & 0xFF)));

    out.push_back(
        static_cast<secs::core::byte>((header.system_bytes >> 24) & 0xFF));
    out.push_back(
        static_cast<secs::core::byte>((header.system_bytes >> 16) & 0xFF));
    out.push_back(
        static_cast<secs::core::byte>((header.system_bytes >> 8) & 0xFF));
    out.push_back(static_cast<secs::core::byte>(header.system_bytes & 0xFF));

    out.insert(out.end(), data.begin(), data.end());

    // 校验和计算范围：长度字段之后的负载（即头部 + 数据）。
    const auto cs = checksum(secs::core::bytes_view{out.data() + 1, length});
    out.push_back(static_cast<secs::core::byte>((cs >> 8) & 0xFF));
    out.push_back(static_cast<secs::core::byte>(cs & 0xFF));

    return {};
}

std::error_code decode_block(secs::core::bytes_view frame, DecodedBlock &out) {
    if (frame.size() > kMaxBlockFrameSize) {
        return make_error_code(errc::invalid_block);
    }
    if (frame.size() < 1 + kHeaderSize + 2) {
        return make_error_code(errc::invalid_block);
    }

    const auto length = static_cast<std::size_t>(frame[0]);
    if (length < kHeaderSize || length > kMaxBlockLength) {
        return make_error_code(errc::invalid_block);
    }

    if (frame.size() != 1 + length + 2) {
        return make_error_code(errc::invalid_block);
    }

    const auto payload = frame.subspan(1, length);
    const auto cs_recv = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[1 + length]) << 8) |
        static_cast<std::uint16_t>(frame[1 + length + 1]));
    const auto cs_calc = checksum(payload);
    if (cs_recv != cs_calc) {
        return make_error_code(errc::checksum_mismatch);
    }

    const auto b1 = payload[0];
    const auto b2 = payload[1];
    const auto b3 = payload[2];
    const auto b4 = payload[3];
    const auto b5 = payload[4];
    const auto b6 = payload[5];

    // 对齐 c_dump：Byte5 低 7 位为保留位（必须为 0）。
    if ((b5 & 0x7F) != 0) {
        return make_error_code(errc::invalid_block);
    }

    Header header{};
    header.reverse_bit = (b1 & 0x80) != 0;
    header.device_id = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(b1 & 0x7F)) << 8) |
        static_cast<std::uint16_t>(b2));

    header.wait_bit = (b3 & 0x80) != 0;
    header.stream = static_cast<std::uint8_t>(b3 & 0x7F);
    header.function = b4;

    header.end_bit = (b5 & 0x80) != 0;
    header.block_number = static_cast<std::uint16_t>(b6);

    header.system_bytes = (static_cast<std::uint32_t>(payload[6]) << 24) |
                          (static_cast<std::uint32_t>(payload[7]) << 16) |
                          (static_cast<std::uint32_t>(payload[8]) << 8) |
                          static_cast<std::uint32_t>(payload[9]);

    out.header = header;
    out.data = payload.subspan(kHeaderSize, length - kHeaderSize);
    return {};
}

std::vector<std::vector<secs::core::byte>>
fragment_message(Header base_header, secs::core::bytes_view payload) {
    std::vector<std::vector<secs::core::byte>> out;

    // 对齐 c_dump：BlockNumber 为 8 位，单条消息最多 255 个 Block（起始为 1）。
    if (!payload.empty()) {
        const auto blocks =
            (payload.size() + kMaxBlockDataSize - 1) / kMaxBlockDataSize;
        if (blocks > 0x00FFu) {
            return out;
        }
    }

    if (payload.empty()) {
        base_header.block_number = 1;
        base_header.end_bit = true;
        out.emplace_back();
        (void)encode_block(base_header, secs::core::bytes_view{}, out.back());
        return out;
    }

    std::size_t offset = 0;
    std::uint16_t block_number = 1;

    while (offset < payload.size()) {
        const auto remaining = payload.size() - offset;
        const auto chunk = std::min<std::size_t>(remaining, kMaxBlockDataSize);
        const auto is_last = (offset + chunk) == payload.size();

        auto hdr = base_header;
        hdr.block_number = block_number;
        hdr.end_bit = is_last;

        out.emplace_back();
        (void)encode_block(hdr, payload.subspan(offset, chunk), out.back());

        offset += chunk;
        ++block_number;
    }

    return out;
}

Reassembler::Reassembler(std::optional<std::uint16_t> expected_device_id)
    : expected_device_id_(expected_device_id) {}

void Reassembler::reset() noexcept {
    has_header_ = false;
    header_ = Header{};
    next_block_ = 1;
    body_.clear();
}

bool Reassembler::has_message() const noexcept {
    return has_header_ && header_.end_bit;
}

const Header &Reassembler::message_header() const noexcept { return header_; }

secs::core::bytes_view Reassembler::message_body() const noexcept {
    return secs::core::bytes_view{body_.data(), body_.size()};
}

std::error_code Reassembler::accept(const DecodedBlock &block) {
    if (!has_header_) {
        if (expected_device_id_.has_value() &&
            block.header.device_id != *expected_device_id_) {
            return make_error_code(errc::device_id_mismatch);
        }
        if (block.header.block_number != 1) {
            return make_error_code(errc::block_sequence_error);
        }
        header_ = block.header;
        header_.end_bit = block.header.end_bit;
        has_header_ = true;
        next_block_ = static_cast<std::uint16_t>(block.header.block_number + 1);
        body_.assign(block.data.begin(), block.data.end());
        return {};
    }

    if (expected_device_id_.has_value() &&
        block.header.device_id != *expected_device_id_) {
        return make_error_code(errc::device_id_mismatch);
    }
    if (block.header.device_id != header_.device_id) {
        return make_error_code(errc::device_id_mismatch);
    }
    if (block.header.system_bytes != header_.system_bytes) {
        return make_error_code(errc::protocol_error);
    }
    if (block.header.stream != header_.stream ||
        block.header.function != header_.function) {
        return make_error_code(errc::protocol_error);
    }
    if (block.header.reverse_bit != header_.reverse_bit ||
        block.header.wait_bit != header_.wait_bit) {
        return make_error_code(errc::protocol_error);
    }
    if (block.header.block_number != next_block_) {
        return make_error_code(errc::block_sequence_error);
    }

    body_.insert(body_.end(), block.data.begin(), block.data.end());
    header_.end_bit = block.header.end_bit;
    next_block_ = static_cast<std::uint16_t>(next_block_ + 1);
    return {};
}

} // namespace secs::secs1
