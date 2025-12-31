#include "secs/sml/runtime.hpp"

#include <charconv>
#include <cstdlib>

namespace secs::sml {

std::error_code Runtime::load(std::string_view source) noexcept {
  auto result = parse_sml(source);
  if (result.ec) {
    return result.ec;
  }

  load(std::move(result.document));
  return {};
}

void Runtime::load(Document doc) noexcept {
  document_ = std::move(doc);
  build_index();
  loaded_ = true;
}

void Runtime::build_index() noexcept {
  name_index_.clear();
  sf_index_.clear();

  for (std::size_t i = 0; i < document_.messages.size(); ++i) {
    const auto& msg = document_.messages[i];

    // 按名称索引
    if (!msg.name.empty()) {
      name_index_[msg.name] = i;
    }

    // 按 Stream/Function 索引（仅匿名消息）
    if (msg.name.empty()) {
      std::uint16_t key = (static_cast<std::uint16_t>(msg.stream) << 8) |
                          static_cast<std::uint16_t>(msg.function);
      sf_index_[key] = i;
    }
  }
}

const MessageDef* Runtime::get_message(std::string_view name) const noexcept {
  auto it = name_index_.find(std::string(name));
  if (it != name_index_.end()) {
    return &document_.messages[it->second];
  }
  return nullptr;
}

const MessageDef* Runtime::get_message(std::uint8_t stream, std::uint8_t function) const noexcept {
  // 先尝试按 SF 查找匿名消息
  std::uint16_t key = (static_cast<std::uint16_t>(stream) << 8) |
                      static_cast<std::uint16_t>(function);
  auto it = sf_index_.find(key);
  if (it != sf_index_.end()) {
    return &document_.messages[it->second];
  }

  // 遍历查找命名消息
  for (const auto& msg : document_.messages) {
    if (msg.stream == stream && msg.function == function) {
      return &msg;
    }
  }

  return nullptr;
}

std::optional<std::string> Runtime::match_response(
    std::uint8_t stream,
    std::uint8_t function,
    const ii::Item& item) const noexcept {
  for (const auto& rule : document_.conditions) {
    if (match_condition(rule.condition, stream, function, item)) {
      return rule.response_name;
    }
  }
  return std::nullopt;
}

bool Runtime::match_condition(const Condition& cond, std::uint8_t stream,
                              std::uint8_t function, const ii::Item& item) const noexcept {
  // 检查消息名是否匹配
  // 条件可以是消息名（如 s1f1），也可以直接写成 SxFy 格式（如 S1F1）

  // 尝试解析为 SxFy
  std::uint8_t cond_stream = 0, cond_function = 0;
  bool is_sf = false;

  std::string_view name = cond.message_name;
  if (name.size() >= 4 && (name[0] == 'S' || name[0] == 's')) {
    std::size_t f_pos = name.find_first_of("Ff");
    if (f_pos != std::string_view::npos && f_pos >= 2) {
      int s = 0, f = 0;
      auto [ptr1, ec1] = std::from_chars(name.data() + 1, name.data() + f_pos, s);
      auto [ptr2, ec2] = std::from_chars(name.data() + f_pos + 1, name.data() + name.size(), f);
      if (ec1 == std::errc{} && ec2 == std::errc{}) {
        cond_stream = static_cast<std::uint8_t>(s);
        cond_function = static_cast<std::uint8_t>(f);
        is_sf = true;
      }
    }
  }

  // 如果是 SxFy 格式，直接比较
  if (is_sf) {
    if (stream != cond_stream || function != cond_function) {
      return false;
    }
  } else {
    // 按消息名查找
    const MessageDef* msg = get_message(cond.message_name);
    if (!msg || msg->stream != stream || msg->function != function) {
      return false;
    }
  }

  // 如果有索引和期望值，检查 Item
  if (cond.index && cond.expected) {
    // 获取指定索引的元素
    auto* list = item.get_if<ii::List>();
    if (!list) {
      return false;
    }

    std::size_t idx = *cond.index;
    if (idx < 1 || idx > list->size()) {
      return false;
    }

    // 比较元素
    const ii::Item& elem = (*list)[idx - 1];  // 约定：SML 中索引从 1 开始
    if (!items_equal(elem, *cond.expected)) {
      return false;
    }
  }

  return true;
}

bool Runtime::items_equal(const ii::Item& a, const ii::Item& b) const noexcept {
  // 简单的类型和值比较
  // 这里只实现基本类型的比较

  // F4 比较
  auto* af4 = a.get_if<ii::F4>();
  auto* bf4 = b.get_if<ii::F4>();
  if (af4 && bf4) {
    if (af4->values.size() != bf4->values.size()) return false;
    for (std::size_t i = 0; i < af4->values.size(); ++i) {
      // 浮点数近似比较
      float diff = af4->values[i] - bf4->values[i];
      if (diff < -0.0001f || diff > 0.0001f) return false;
    }
    return true;
  }

  // F8 比较
  auto* af8 = a.get_if<ii::F8>();
  auto* bf8 = b.get_if<ii::F8>();
  if (af8 && bf8) {
    if (af8->values.size() != bf8->values.size()) return false;
    for (std::size_t i = 0; i < af8->values.size(); ++i) {
      double diff = af8->values[i] - bf8->values[i];
      if (diff < -0.0001 || diff > 0.0001) return false;
    }
    return true;
  }

  // U2 比较
  auto* au2 = a.get_if<ii::U2>();
  auto* bu2 = b.get_if<ii::U2>();
  if (au2 && bu2) {
    return au2->values == bu2->values;
  }

  // U4 比较
  auto* au4 = a.get_if<ii::U4>();
  auto* bu4 = b.get_if<ii::U4>();
  if (au4 && bu4) {
    return au4->values == bu4->values;
  }

  // ASCII 比较
  auto* aa = a.get_if<ii::ASCII>();
  auto* ba = b.get_if<ii::ASCII>();
  if (aa && ba) {
    return aa->value == ba->value;
  }

  // 其他类型暂不支持
  return false;
}

}  // namespace secs::sml
