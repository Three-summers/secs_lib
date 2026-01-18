# SML + CEID 完整示例说明

## 概述

本示例展示如何结合 **SML 模板**、**CEID dispatcher** 和**变量注入**，实现一个灵活的设备状态查询系统。

## 核心特性

### 1. SML 模板定义（带占位符）

```sml
status_response: S6F12
<L
  <U2 DATAID>           /* 变量：从请求中提取 */
  <U2 0x1001>           /* CEID 回显 */
  <L
    <A DEVICE_NAME>     /* 变量：设备名称 */
    <U1 STATUS_CODE>    /* 变量：状态码 */
    <U4 UPTIME_SECONDS> /* 变量：运行时间 */
  >
>.
```

### 2. 条件响应规则（根据 CEID 自动选择）

```sml
/* 条件中的 s6f11 会被自动解析为 stream=6, function=11 */
if (s6f11(3)==<U2 0x1001>) status_response.
if (s6f11(3)==<U2 0x1002>) temperature_response.
if (s6f11(3)==<U2 0x1003>) alarm_response.
if (s6f11(3)==<U2 0x1004>) production_response.
```

**关键点：**
- 索引 `(3)` 采用**先序遍历编号**（包含根节点），从 1 开始
- S6F11 结构: `<L <U2 DATAID> <U2 CEID> <L>>`
  - (1) = 根节点 List
  - (2) = `<U2 DATAID>`
  - (3) = `<U2 CEID>` ← CEID 在这里
  - (4) = `<L>` params

### 3. 运行时变量注入

```cpp
// 根据 CEID 填充不同的数据
sml::RenderContext ctx;
ctx.set("DATAID", ii::Item::u2({dataid}));

switch (ceid) {
case 0x1001: // 设备状态查询
    ctx.set("DEVICE_NAME", ii::Item::ascii(data.device_name));
    ctx.set("STATUS_CODE", ii::Item::u1({data.status_code}));
    ctx.set("UPTIME_SECONDS", ii::Item::u4({data.uptime_seconds}));
    break;

case 0x1002: // 温度数据查询
    ctx.set("TEMP_SENSOR_1", ii::Item::f4({data.temp_sensor_1}));
    ctx.set("TEMP_SENSOR_2", ii::Item::f4({data.temp_sensor_2}));
    ctx.set("TEMP_SENSOR_3", ii::Item::f4({data.temp_sensor_3}));
    break;

// ... 其他 CEID
}
```

### 4. 自动渲染并回包

```cpp
// SML Runtime 自动匹配条件并返回响应模板名称
auto response_name = rt.match_response(req.stream, req.function, decoded.item);

// 渲染模板（注入变量）
std::vector<core::byte> response_body;
auto enc_ec = rt.encode_message_body(*response_name, ctx, response_body);

// 返回给 protocol::Session，自动回包
co_return protocol::HandlerResult{std::error_code{}, std::move(response_body)};
```

## 运行示例

```bash
cd build/examples
./sml_ceid_complete
```

## 输出示例

```
=== Test 1: CEID=0x1001 ===
[Host] Sending S6F11 (W=1), DATAID=1, CEID=0x1001
[Equipment] Received S6F11
[Equipment] DATAID=1, CEID=0x1001
[Equipment] Matched response: status_response
[Equipment] Response rendered, size=36 bytes

[Host] Received S6F12
[Host] Response body:
  <L> (3 items)
    <U2 1>
    <U2 4097>
    <L> (3 items)
      <A "EQUIPMENT-001">
      <U1 1>
      <U4 12345>
```

## 架构优势

### 1. 关注点分离

- **SML 文件**：定义消息结构和路由规则（配置）
- **C++ 代码**：提供动态数据和业务逻辑（代码）

### 2. 易于维护

- 新增 CEID：只需在 SML 文件中添加模板和条件规则
- 修改响应格式：只需修改 SML 模板
- 无需重新编译（SML 文件在运行时加载）

### 3. 类型安全

- 变量类型在渲染时检查
- 编译期保证 SECS-II 编解码正确性

## 适用场景

✅ **推荐使用 SML + CEID 的场景：**
- 多个 CEID，每个 CEID 对应不同的响应格式
- 响应格式相对固定，只有少数字段需要动态填充
- 需要快速迭代和调试（修改 SML 无需重新编译）

❌ **不推荐使用 SML 的场景：**
- 响应逻辑非常复杂（多层条件判断、状态机）
- 需要访问外部资源（数据库、文件系统）
- 响应格式完全动态（无法预先定义模板）

对于复杂场景，直接使用 C++ 的 `CeidDispatcher`：

```cpp
auto disp = std::make_shared<protocol::CeidDispatcher>(extractor);
disp->set(0x1001, [](auto ceid, auto& item, auto& msg) {
    // 复杂逻辑在这里
    return build_complex_response(ceid, item);
});
```

## 文件清单

- `sml_ceid_complete.cpp` - 完整的 C++ 示例代码
- `sml_ceid_complete.sml` - SML 模板文件
- `SML_CEID_README.md` - 本说明文档

## 关键技术点总结

1. **SML 条件匹配**：`if (s6f11(3)==<U2 0x1001>)` 自动解析 stream/function
2. **先序遍历索引**：理解 `(3)` 的含义（包含根节点）
3. **变量注入**：`RenderContext` 提供运行时数据
4. **自动渲染**：`Runtime::encode_message_body()` 一步完成渲染+编码
5. **Router 集成**：`match_response()` 返回响应模板名称

## 扩展阅读

- `docs/architecture/09-smlx-extension.md` - SMLX 扩展设计文档
- `docs/architecture/06-sml-module.md` - SML 模块说明
- `include/secs/protocol/ceid_dispatcher.hpp` - CEID dispatcher API
- `examples/ceid_dispatcher_tvoc_style.cpp` - 纯 C++ 的 CEID 示例
