#pragma once

/**
 * @brief 标准/常用消息类型集合（可选）。
 *
 * 说明：
 * - 这些类型仅提供 from_item/to_item（用于配合 protocol::TypedHandler 等），不绑定
 *   任何具体的 Host/Equipment 业务状态机；
 * - 若你的设备/Host 有厂商自定义扩展，建议在业务侧按相同模式扩展更多 SxFy 类型。
 */

#include "secs/messages/s1.hpp"
#include "secs/messages/s2.hpp"

