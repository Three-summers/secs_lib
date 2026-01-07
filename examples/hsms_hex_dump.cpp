/**
 * @file hsms_hex_dump.cpp
 * @brief 输出 HSMS 消息的 16 进制格式示例
 */

#include <secs/hsms/message.hpp>
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace secs;

// 辅助函数：输出 16 进制数据
void print_hex(const std::vector<core::byte> &data, const std::string &label) {
    std::cout << "\n=== " << label << " ===\n";
    std::cout << "总长度: " << data.size() << " 字节\n\n";

    // 打印 16 进制
    std::cout << "16进制表示:\n";
    for (size_t i = 0; i < data.size(); ++i) {
        if (i % 16 == 0) {
            std::cout << std::setw(4) << std::setfill('0') << std::hex << i
                      << ": ";
        }
        std::cout << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[i]) << " ";
        if ((i + 1) % 16 == 0) {
            std::cout << "\n";
        }
    }
    if (data.size() % 16 != 0) {
        std::cout << "\n";
    }

    // 打印解析
    std::cout << "\n字段解析:\n";
    if (data.size() >= 14) {
        std::cout << "  [0-3]   长度字段: 0x";
        for (int i = 0; i < 4; i++) {
            std::cout << std::setw(2) << std::setfill('0') << std::hex
                      << static_cast<int>(data[i]);
        }
        std::cout << " (" << std::dec
                  << ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3])
                  << " 字节)\n";

        std::cout << "  [4-5]   Session ID: 0x"
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[4])
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[5]) << "\n";

        std::cout << "  [6]     Stream/W-bit: 0x"
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[6]);
        bool w_bit = (data[6] & 0x80) != 0;
        uint8_t stream = data[6] & 0x7F;
        std::cout << " (W=" << (w_bit ? "1" : "0")
                  << ", Stream=" << std::dec << static_cast<int>(stream) << ")\n";

        std::cout << "  [7]     Function: 0x"
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[7])
                  << " (F" << std::dec << static_cast<int>(data[7]) << ")\n";

        std::cout << "  [8]     P-Type: 0x"
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[8]) << " (SECS-II)\n";

        std::cout << "  [9]     S-Type: 0x"
                  << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[9]);
        const char* stype_name = "Unknown";
        switch(data[9]) {
            case 0x00: stype_name = "Data"; break;
            case 0x01: stype_name = "Select.req"; break;
            case 0x02: stype_name = "Select.rsp"; break;
            case 0x05: stype_name = "Linktest.req"; break;
            case 0x06: stype_name = "Linktest.rsp"; break;
        }
        std::cout << " (" << stype_name << ")\n";

        std::cout << "  [10-13] System Bytes: 0x";
        for (int i = 10; i < 14; i++) {
            std::cout << std::setw(2) << std::setfill('0') << std::hex
                      << static_cast<int>(data[i]);
        }
        std::cout << "\n";

        if (data.size() > 14) {
            std::cout << "  [14+]   消息体: " << std::dec << (data.size() - 14)
                      << " 字节\n";
        }
    }
    std::cout << "\n";
}

int main() {
    std::cout << "=== HSMS 消息 16 进制示例 ===\n";

    // 1. SELECT.req 消息
    {
        auto msg = hsms::make_select_req(0xFFFF, 0x00000001);
        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "SELECT.req");
    }

    // 2. SELECT.rsp 消息（状态=0，接受）
    {
        auto msg = hsms::make_select_rsp(0xFFFF, 0x00, 0x00000001);
        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "SELECT.rsp");
    }

    // 3. LINKTEST.req 消息
    {
        auto msg = hsms::make_linktest_req(0xFFFF, 0x00000002);
        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "LINKTEST.req");
    }

    // 4. LINKTEST.rsp 消息
    {
        auto msg = hsms::make_linktest_rsp(0xFFFF, 0x00000002);
        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "LINKTEST.rsp");
    }

    // 5. S1F1 数据消息（Are You There）- 空 List
    {
        ii::Item request = ii::Item::list({});
        std::vector<core::byte> body;
        ii::encode(request, body);

        auto msg = hsms::make_data_message(
            0x0001,  // Session ID
            1,       // Stream
            1,       // Function
            true,    // W=1 (需要回复)
            0x00000003,  // System Bytes
            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "S1F1 (Are You There) - 请求");
    }

    // 6. S1F2 数据消息（响应）- ASCII 字符串
    {
        ii::Item response = ii::Item::ascii("OK");
        std::vector<core::byte> body;
        ii::encode(response, body);

        auto msg = hsms::make_data_message(
            0x0001,  // Session ID
            1,       // Stream
            2,       // Function
            false,   // W=0 (不需要回复)
            0x00000003,  // System Bytes (与请求相同)
            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "S1F2 (Are You There) - 响应");
    }

    // 7. S2F41 数据消息（Host Command）
    {
        ii::Item command = ii::Item::list({
            ii::Item::ascii("START"),  // RCMD
            ii::Item::list({})         // PARAMS (空)
        });
        std::vector<core::byte> body;
        ii::encode(command, body);

        auto msg = hsms::make_data_message(
            0x0001,  // Session ID
            2,       // Stream
            41,      // Function
            true,    // W=1
            0x00000004,  // System Bytes
            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "S2F41 (Host Command)");
    }

    // 8. S6F11 数据消息（Event Report，单向）
    {
        ii::Item event = ii::Item::list({
            ii::Item::u4({1}),      // DATAID
            ii::Item::u4({100}),    // CEID
            ii::Item::list({})      // RPT (空)
        });
        std::vector<core::byte> body;
        ii::encode(event, body);

        auto msg = hsms::make_data_message(
            0x0001,  // Session ID
            6,       // Stream
            11,      // Function
            false,   // W=0 (单向消息)
            0x00000005,  // System Bytes
            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);
        print_hex(frame, "S6F11 (Event Report) - 单向");
    }

    return 0;
}
