/**
 * @file test_c_dump_secs1_block_compat.cpp
 * @brief 对齐 c_dump 参考实现的 SECS-I Block 编码（字节级一致性）
 *
 * 说明：
 * - c_dump/Secs_App/secs_I.c 在真实环境中跑通过，这里用其组帧逻辑作为参考输出。
 * - 本测试只对齐“Block frame 的字节布局 + checksum 计算”，不涉及串口/定时器。
 */

#include "secs/secs1/block.hpp"

#include "test_main.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include "secs_struct.h"

// c_dump 中未在头文件公开，但在 secs_I.c 里有定义。
int SecsI_ArrangeHead(SecsHead *ArrangeHead, std::uint8_t *Secs_SendBuf);
void SecsI_ArrangeChecksum(std::uint8_t *Secs_SendBuf);

// secs_I.c 里会引用 BSP 层函数；这里提供最小 stub 以满足链接。
int Secs_SerialSend(SecsBSPInfo *, std::uint8_t *, std::uint16_t) { return 0; }
int Secs_TimerCtrl(SecsBSPInfo *, std::uint8_t, std::uint16_t) { return 0; }
}

namespace {

std::vector<std::uint8_t>
encode_block_with_c_dump(const secs::secs1::Header &hdr,
                         const std::vector<std::uint8_t> &data) {
    // c_dump 的 send buffer 是定长 256；这里按实际 frame 长度构造即可。
    const std::size_t length = secs::secs1::kHeaderSize + data.size();
    std::vector<std::uint8_t> out(1 + length + 2, 0);
    out[0] = static_cast<std::uint8_t>(length);

    SecsHead cd{};
    cd.Reverse_Bit = hdr.reverse_bit ? 1u : 0u;
    cd.Device_ID = hdr.device_id;
    cd.Reply_Bit = hdr.wait_bit ? 0x80u : 0u; // c_dump 组包逻辑按“已移位”的值相加
    cd.Secs_Type.Stream_ID = hdr.stream;
    cd.Secs_Type.Function_ID = hdr.function;
    cd.End_Bit = hdr.end_bit ? 1u : 0u;
    cd.Block_num = hdr.block_number;
    cd.SystemByte = hdr.system_bytes;

    (void)SecsI_ArrangeHead(&cd, out.data());

    if (!data.empty()) {
        std::copy(data.begin(), data.end(), out.begin() + 1 + secs::secs1::kHeaderSize);
    }
    SecsI_ArrangeChecksum(out.data());
    return out;
}

std::vector<std::uint8_t>
encode_block_with_cpp(const secs::secs1::Header &hdr,
                      const std::vector<std::uint8_t> &data) {
    std::vector<secs::core::byte> out_bytes;
    TEST_EXPECT_OK(secs::secs1::encode_block(
        hdr,
        secs::core::bytes_view{reinterpret_cast<const secs::core::byte *>(data.data()),
                               data.size()},
        out_bytes));

    std::vector<std::uint8_t> out;
    out.reserve(out_bytes.size());
    for (auto b : out_bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

void expect_same_block(const secs::secs1::Header &hdr,
                       const std::vector<std::uint8_t> &data) {
    const auto expected = encode_block_with_c_dump(hdr, data);
    const auto actual = encode_block_with_cpp(hdr, data);
    TEST_EXPECT_EQ(actual, expected);
}

void test_block_header_and_checksum_match_c_dump() {
    {
        secs::secs1::Header hdr{};
        hdr.reverse_bit = false;
        hdr.device_id = 0x1000;
        hdr.wait_bit = false;
        hdr.stream = 1;
        hdr.function = 2;
        hdr.end_bit = true;
        hdr.block_number = 1;
        hdr.system_bytes = 0x01020304u;

        expect_same_block(hdr, {0x11, 0x22, 0x33});
    }

    {
        secs::secs1::Header hdr{};
        hdr.reverse_bit = true;
        hdr.device_id = 0x7FFF;
        hdr.wait_bit = true;
        hdr.stream = 127;
        hdr.function = 255;
        hdr.end_bit = false;
        hdr.block_number = 255;
        hdr.system_bytes = 0xAABBCCDDu;

        expect_same_block(hdr, {});
    }
}

} // namespace

int main() {
    test_block_header_and_checksum_match_c_dump();
    return secs::tests::run_and_report();
}

