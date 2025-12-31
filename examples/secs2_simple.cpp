#include <iostream>
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>

using namespace secs::ii;

int main() {
    std::cout << "=== SECS-II 编解码简单示例 ===\n\n";

    // 构造 ASCII 消息
    Item msg = Item::ascii("Hello SECS");

    // 编码
    std::vector<byte> encoded;
    auto ec = encode(msg, encoded);
    if (ec) {
        std::cerr << "编码失败\n";
        return 1;
    }

    std::cout << "编码成功: " << encoded.size() << " 字节\n";

    // 解码
    Item decoded{Item::ascii("")}; // 临时初始值
    std::size_t consumed;
    ec = decode_one(
        bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
        std::cerr << "解码失败\n";
        return 1;
    }

    auto *ascii_ptr = decoded.get_if<ASCII>();
    if (ascii_ptr) {
        std::cout << "解码成功: \"" << ascii_ptr->value << "\"\n";
    }

    return 0;
}
