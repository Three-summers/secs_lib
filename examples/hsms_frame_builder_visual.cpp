/**
 * @file hsms_frame_builder_visual.cpp
 * @brief HSMS 帧构建可视化示例 - 逐步展示每一帧的构建过程
 */

#include <secs/hsms/message.hpp>
#include <secs/ii/item.hpp>
#include <secs/utils/ii_helpers.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace secs;

// 函数声明
void parse_secs2_item(const std::vector<core::byte> &data, size_t &offset, const std::string &indent);

// 颜色代码
#define COLOR_RESET   "\033[0m"
#define COLOR_HEADER  "\033[1;36m"  // 青色（头部）
#define COLOR_LENGTH  "\033[1;33m"  // 黄色（长度字段）
#define COLOR_BODY    "\033[1;35m"  // 紫色（消息体）
#define COLOR_LABEL   "\033[1;32m"  // 绿色（标签）
#define COLOR_VALUE   "\033[1;37m"  // 白色（数值）

void print_separator() {
    std::cout << "\n" << std::string(80, '=') << "\n\n";
}

void print_hex_bytes(const std::vector<core::byte> &data, size_t start, size_t count, const char* color) {
    std::cout << color;
    for (size_t i = start; i < start + count && i < data.size(); ++i) {
        std::cout << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(data[i]) << " ";
    }
    std::cout << COLOR_RESET;
}

void print_frame_visual(const std::vector<core::byte> &frame, const std::string &title) {
    std::cout << COLOR_LABEL << "┌─ " << title << " ─┐" << COLOR_RESET << "\n\n";

    if (frame.size() < 14) {
        std::cout << "错误：帧长度不足\n";
        return;
    }

    // 1. 显示完整帧
    std::cout << COLOR_LABEL << "完整帧（" << std::dec << frame.size() << " 字节）：" << COLOR_RESET << "\n";
    for (size_t i = 0; i < frame.size(); ++i) {
        // 根据位置选择颜色
        const char* color = COLOR_RESET;
        if (i < 4) color = COLOR_LENGTH;           // 长度字段
        else if (i < 14) color = COLOR_HEADER;     // 头部
        else color = COLOR_BODY;                   // 消息体

        std::cout << color << std::setw(2) << std::setfill('0') << std::hex
                  << static_cast<int>(frame[i]) << " " << COLOR_RESET;

        if ((i + 1) % 16 == 0) std::cout << "\n";
    }
    if (frame.size() % 16 != 0) std::cout << "\n";

    // 2. 分段解析
    std::cout << "\n" << COLOR_LABEL << "字段分解：" << COLOR_RESET << "\n\n";

    // 长度字段
    std::cout << COLOR_LENGTH << "[0-3] 长度字段: " << COLOR_RESET;
    print_hex_bytes(frame, 0, 4, COLOR_LENGTH);
    uint32_t payload_len = (frame[0] << 24) | (frame[1] << 16) | (frame[2] << 8) | frame[3];
    std::cout << COLOR_VALUE << " → " << std::dec << payload_len << " 字节" << COLOR_RESET << "\n";

    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET
              << "write_u32_be(payload_len=" << std::dec << payload_len << ")\n";

    // Session ID
    std::cout << "\n" << COLOR_HEADER << "[4-5] Session ID: " << COLOR_RESET;
    print_hex_bytes(frame, 4, 2, COLOR_HEADER);
    uint16_t session_id = (frame[4] << 8) | frame[5];
    std::cout << COLOR_VALUE << " → 0x" << std::hex << std::setw(4) << std::setfill('0')
              << session_id << COLOR_RESET;
    if (session_id == 0xFFFF) {
        std::cout << " (控制消息)";
    } else {
        std::cout << " (Device ID=" << std::dec << session_id << ")";
    }
    std::cout << "\n";
    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET
              << "header.session_id = 0x" << std::hex << session_id << "\n";

    // Header Byte 2
    std::cout << "\n" << COLOR_HEADER << "[6]   Byte 2:     " << COLOR_RESET;
    print_hex_bytes(frame, 6, 1, COLOR_HEADER);
    uint8_t byte2 = frame[6];
    bool w_bit = (byte2 & 0x80) != 0;
    uint8_t stream = byte2 & 0x7F;
    std::cout << COLOR_VALUE << " → 0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(byte2) << COLOR_RESET;

    if (frame[9] == 0x00) {  // Data message
        std::cout << " (W=" << (w_bit ? "1" : "0")
                  << ", Stream=" << std::dec << static_cast<int>(stream) << ")";
        std::cout << "\n      " << COLOR_LABEL << "构建：" << COLOR_RESET
                  << "header_byte2 = (w_bit << 7) | stream = ("
                  << (w_bit ? "1" : "0") << " << 7) | " << std::dec << static_cast<int>(stream)
                  << " = 0x" << std::hex << static_cast<int>(byte2) << "\n";
    } else {
        std::cout << " (状态/原因码)";
        std::cout << "\n      " << COLOR_LABEL << "构建：" << COLOR_RESET
                  << "header_byte2 = " << std::dec << static_cast<int>(byte2) << "\n";
    }

    // Function
    std::cout << "\n" << COLOR_HEADER << "[7]   Function:   " << COLOR_RESET;
    print_hex_bytes(frame, 7, 1, COLOR_HEADER);
    uint8_t function = frame[7];
    std::cout << COLOR_VALUE << " → F" << std::dec << static_cast<int>(function) << COLOR_RESET << "\n";
    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET
              << "header_byte3 = " << std::dec << static_cast<int>(function) << "\n";

    // P-Type
    std::cout << "\n" << COLOR_HEADER << "[8]   P-Type:     " << COLOR_RESET;
    print_hex_bytes(frame, 8, 1, COLOR_HEADER);
    std::cout << COLOR_VALUE << " → SECS-II (0x00)" << COLOR_RESET << "\n";
    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET << "p_type = 0x00\n";

    // S-Type
    std::cout << "\n" << COLOR_HEADER << "[9]   S-Type:     " << COLOR_RESET;
    print_hex_bytes(frame, 9, 1, COLOR_HEADER);
    const char* stype_name = "Unknown";
    switch(frame[9]) {
        case 0x00: stype_name = "Data"; break;
        case 0x01: stype_name = "Select.req"; break;
        case 0x02: stype_name = "Select.rsp"; break;
        case 0x05: stype_name = "Linktest.req"; break;
        case 0x06: stype_name = "Linktest.rsp"; break;
    }
    std::cout << COLOR_VALUE << " → " << stype_name << COLOR_RESET << "\n";
    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET
              << "s_type = SType::" << stype_name << " (0x"
              << std::hex << static_cast<int>(frame[9]) << ")\n";

    // System Bytes
    std::cout << "\n" << COLOR_HEADER << "[10-13] System Bytes: " << COLOR_RESET;
    print_hex_bytes(frame, 10, 4, COLOR_HEADER);
    uint32_t sys_bytes = (frame[10] << 24) | (frame[11] << 16) | (frame[12] << 8) | frame[13];
    std::cout << COLOR_VALUE << " → 0x" << std::hex << std::setw(8) << std::setfill('0')
              << sys_bytes << COLOR_RESET << " (事务ID)\n";
    std::cout << "      " << COLOR_LABEL << "构建：" << COLOR_RESET
              << "system_bytes = 0x" << std::hex << sys_bytes << "\n";

    // 消息体
    if (frame.size() > 14) {
        size_t body_size = frame.size() - 14;
        std::cout << "\n" << COLOR_BODY << "[14+] 消息体 (" << std::dec << body_size << " 字节):"
                  << COLOR_RESET << "\n";
        std::cout << "      ";
        print_hex_bytes(frame, 14, body_size, COLOR_BODY);
        std::cout << "\n";

        // 解析 SECS-II 结构
        if (body_size >= 2) {
            std::cout << "\n" << COLOR_LABEL << "      SECS-II 解析：" << COLOR_RESET << "\n";
            size_t offset = 14;
            parse_secs2_item(frame, offset, "      ");
        }
    } else {
        std::cout << "\n" << COLOR_BODY << "[消息体] 空" << COLOR_RESET << "\n";
    }

    std::cout << "\n" << COLOR_LABEL << "└" << std::string(78, '─') << "┘" << COLOR_RESET << "\n";
}

void parse_secs2_item(const std::vector<core::byte> &data, size_t &offset, const std::string &indent) {
    if (offset >= data.size()) return;

    // Format Byte
    uint8_t format_byte = data[offset];
    uint8_t format_code = format_byte >> 2;
    uint8_t length_bytes = format_byte & 0x03;

    std::cout << indent << "Format Byte: " << COLOR_BODY
              << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(format_byte)
              << COLOR_RESET << " → ";
    std::cout << "code=" << std::hex << static_cast<int>(format_code)
              << ", len_bytes=" << std::dec << static_cast<int>(length_bytes) << "\n";

    std::cout << indent << COLOR_LABEL << "构建：" << COLOR_RESET
              << "format_byte = (" << std::hex << static_cast<int>(format_code)
              << " << 2) | " << std::dec << static_cast<int>(length_bytes)
              << " = 0x" << std::hex << static_cast<int>(format_byte) << "\n";

    offset++;

    // Length
    if (offset + length_bytes > data.size()) return;

    uint32_t length = 0;
    std::cout << indent << "Length: " << COLOR_BODY;
    for (size_t i = 0; i < length_bytes; ++i) {
        length = (length << 8) | data[offset + i];
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(data[offset + i]) << " ";
    }
    std::cout << COLOR_RESET << "→ " << std::dec << length;

    const char* type_name = "Unknown";
    switch (format_code) {
        case 0x00: type_name = "List (子元素数)"; break;
        case 0x10: type_name = "ASCII (字节数)"; break;
        case 0x08: type_name = "Binary (字节数)"; break;
        case 0x2C: type_name = "U4 (字节数)"; break;
        default: type_name = "(字节数)"; break;
    }
    std::cout << " " << type_name << "\n";

    offset += length_bytes;

    // Payload 简要显示
    if (format_code == 0x00) {  // List
        std::cout << indent << COLOR_LABEL << "→ List 包含 " << std::dec << length
                  << " 个子元素" << COLOR_RESET << "\n";
        for (uint32_t i = 0; i < length && offset < data.size(); ++i) {
            std::cout << indent << "  [" << i << "] ";
            parse_secs2_item(data, offset, indent + "      ");
        }
    } else if (format_code == 0x10) {  // ASCII
        std::cout << indent << "Payload: \"";
        for (uint32_t i = 0; i < length && offset + i < data.size(); ++i) {
            char ch = static_cast<char>(data[offset + i]);
            std::cout << (std::isprint(ch) ? ch : '?');
        }
        std::cout << "\"\n";
        offset += length;
    } else {
        std::cout << indent << "Payload (" << std::dec << length << " bytes): ";
        size_t show = std::min(static_cast<size_t>(length), size_t(16));
        for (size_t i = 0; i < show && offset + i < data.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(data[offset + i]) << " ";
        }
        if (length > 16) std::cout << "...";
        std::cout << "\n";
        offset += length;
    }
}

int main() {
    std::cout << "\n";
    std::cout << COLOR_LABEL << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           HSMS 报文构建详细可视化 - 逐步解析每一帧的构建过程              ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝"
              << COLOR_RESET << "\n";

    print_separator();

    // 1. SELECT.req
    {
        std::cout << COLOR_LABEL << "示例 1：SELECT.req（连接握手请求）\n" << COLOR_RESET;
        std::cout << "代码：hsms::make_select_req(0xFFFF, 0x00000001)\n";

        auto msg = hsms::make_select_req(0xFFFF, 0x00000001);
        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);

        print_frame_visual(frame, "SELECT.req");
        print_separator();
    }

    // 2. S1F1
	    {
	        std::cout << COLOR_LABEL << "示例 2：S1F1（Are You There）- 空 List\n" << COLOR_RESET;
	        std::cout << "代码：\n";
	        std::cout << "  ii::Item request = ii::Item::list({});\n";
	        std::cout << "  auto [enc_ec, body] = secs::utils::encode_item(request);\n";
	        std::cout << "  make_data_message(0x0001, 1, 1, true, 0x00000003, body)\n";
	
	        ii::Item request = ii::Item::list({});
	        auto [enc_ec, body] = secs::utils::encode_item(request);
	        if (enc_ec) {
	            std::cerr << "SECS-II encode failed: " << enc_ec.message() << "\n";
	            return 1;
	        }
	
	        auto msg = hsms::make_data_message(
	            0x0001, 1, 1, true, 0x00000003,
	            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);

        print_frame_visual(frame, "S1F1 - Are You There");
        print_separator();
    }

    // 3. S1F2
	    {
	        std::cout << COLOR_LABEL << "示例 3：S1F2（响应）- ASCII \"OK\"\n" << COLOR_RESET;
	        std::cout << "代码：\n";
	        std::cout << "  ii::Item response = ii::Item::ascii(\"OK\");\n";
	        std::cout << "  auto [enc_ec, body] = secs::utils::encode_item(response);\n";
	        std::cout << "  make_data_message(0x0001, 1, 2, false, 0x00000003, body)\n";
	
	        ii::Item response = ii::Item::ascii("OK");
	        auto [enc_ec, body] = secs::utils::encode_item(response);
	        if (enc_ec) {
	            std::cerr << "SECS-II encode failed: " << enc_ec.message() << "\n";
	            return 1;
	        }
	
	        auto msg = hsms::make_data_message(
	            0x0001, 1, 2, false, 0x00000003,
	            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);

        print_frame_visual(frame, "S1F2 - Response");
        print_separator();
    }

    // 4. S2F41
	    {
	        std::cout << COLOR_LABEL << "示例 4：S2F41（Host Command）- 嵌套 List\n" << COLOR_RESET;
	        std::cout << "代码：\n";
	        std::cout << "  ii::Item command = ii::Item::list({\n";
	        std::cout << "      ii::Item::ascii(\"START\"),\n";
	        std::cout << "      ii::Item::list({})\n";
	        std::cout << "  });\n";
	        std::cout << "  auto [enc_ec, body] = secs::utils::encode_item(command);\n";
	
	        ii::Item command = ii::Item::list({
	            ii::Item::ascii("START"),
	            ii::Item::list({})
	        });
	        auto [enc_ec, body] = secs::utils::encode_item(command);
	        if (enc_ec) {
	            std::cerr << "SECS-II encode failed: " << enc_ec.message() << "\n";
	            return 1;
	        }
	
	        auto msg = hsms::make_data_message(
	            0x0001, 2, 41, true, 0x00000004,
	            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        hsms::encode_frame(msg, frame);

        print_frame_visual(frame, "S2F41 - Host Command");
        print_separator();
    }

    std::cout << COLOR_LABEL << "\n✓ 所有示例已完成\n" << COLOR_RESET;
    std::cout << "\n详细文档：HSMS报文构建详解.md\n\n";

    return 0;
}
