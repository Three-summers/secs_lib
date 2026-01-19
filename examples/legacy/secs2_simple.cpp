#include <iostream>
#include <secs/ii/item.hpp>
#include <secs/utils/ii_helpers.hpp>

using namespace secs::ii;

int main() {
    std::cout << "=== SECS-II 编解码简单示例 ===\n\n";

    // 构造 ASCII 消息
    Item msg = Item::ascii("Hello SECS");

    // 编码
    auto [enc_ec, encoded] = secs::utils::encode_item(msg);
    if (enc_ec) {
        std::cerr << "编码失败\n";
        return 1;
    }

    std::cout << "编码成功: " << encoded.size() << " 字节\n";

    // 解码
    auto [dec_ec, decoded] = secs::utils::decode_one_item(
        secs::core::bytes_view{encoded.data(), encoded.size()});
    if (dec_ec) {
        std::cerr << "解码失败\n";
        return 1;
    }

    auto *ascii_ptr = decoded.item.get_if<ASCII>();
    if (ascii_ptr) {
        std::cout << "解码成功: \"" << ascii_ptr->value << "\"\n";
    }

    return 0;
}
