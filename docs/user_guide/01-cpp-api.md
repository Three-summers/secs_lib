# C++ API 使用指南

> 文档更新：2026-01-19
> 目标读者：需要集成 secs_lib 的 C++ 开发者

本库提供两种截然不同的开发模式，根据业务场景选择：

| 路线 | 适用场景 | 核心特征 |
| :--- | :--- | :--- |
| **路线一：编程模式** | 复杂业务逻辑、数据库交互、设备控制 | **Code-First**，类型安全，逻辑在 C++ 代码中 |
| **路线二：SMLX 模式** | 仿真器、快速原型、频繁变动的简单报文 | **Rule-Based**，逻辑在 SML 文件中，支持热加载 |

---

## 路线一：编程模式 (Code-First)

标准开发模式。你需要定义消息结构体并编写 Handler 处理逻辑。

### 1. 定义消息结构

#### 方法 A：声明式定义（推荐）

利用静态反射 (`secs_members`)，只需声明成员顺序，库自动处理编解码。

**场景 1：固定结构**
S1F2: `<L <A MDLN> <A SOFTREV>>`

```cpp
#include <secs/ii/struct_codec.hpp>

struct S1F2Response {
    std::string mdln;
    std::string softrev;

    // 声明成员指针，顺序即为 SECS-II List 中的顺序
    static constexpr auto secs_members() {
        return std::make_tuple(&S1F2Response::mdln, &S1F2Response::softrev);
    }
};
```

**场景 2：变长列表**
S2F13: `<L <U4 ECID1> <U4 ECID2> ...>`

```cpp
struct S2F13Request {
    std::vector<uint32_t> ecids;
    static constexpr auto secs_members() {
        return std::make_tuple(&S2F13Request::ecids);
    }
};
```

**场景 3：嵌套结构**
S5F1: `<L <L ALID ALTX> <L ALID ALTX> ...>`

```cpp
struct AlarmInfo {
    uint32_t alid;
    std::string altx;
    static constexpr auto secs_members() {
        return std::make_tuple(&AlarmInfo::alid, &AlarmInfo::altx);
    }
};

struct S5F1Request {
    std::vector<AlarmInfo> alarms;
    static constexpr auto secs_members() {
        return std::make_tuple(&S5F1Request::alarms);
    }
};
```

#### 方法 B：手动定义（底层控制）

当标准映射无法满足需求时（如非标准 List 结构、复杂位操作、特殊枚举转换），手动实现 `to_item` 和 `from_item`。

```cpp
#include <secs/ii/item.hpp>

struct CustomMsg {
    int id;
    std::string val;

    // 编码：C++ -> Item
    secs::ii::Item to_item() const {
        return secs::ii::Item::list({
            secs::ii::Item::i4({id}),
            secs::ii::Item::ascii(val)
        });
    }

    // 解码：Item -> C++
    static std::optional<CustomMsg> from_item(const secs::ii::Item& item) {
        auto* list = item.get_if<secs::ii::List>();
        if (!list || list->size() != 2) return std::nullopt;

        auto* i4 = (*list)[0].get_if<secs::ii::I4>();
        auto* ascii = (*list)[1].get_if<secs::ii::ASCII>();
        if (!i4 || i4->values.size() != 1 || !ascii) return std::nullopt;

        return CustomMsg{i4->values[0], ascii->value};
    }
};
```

### 2. 编写与注册 Handler

使用 `TypedHandler` 实现类型安全的消息处理。

```cpp
#include <secs/protocol/typed_handler.hpp>

// 定义 Handler：接收 Request，返回 Response
class MyS1F1Handler : public secs::protocol::TypedHandler<S1F1Request, S1F2Response> {
public:
    // 构造函数可用于传入业务指针（如数据库连接）
    MyS1F1Handler(DeviceController* dev) : dev_(dev) {}

    asio::awaitable<std::pair<std::error_code, S1F2Response>>
    handle(const S1F1Request& req, const secs::protocol::DataMessage& raw) override {
        S1F2Response rsp;
        rsp.mdln = dev_->get_model_name();
        rsp.softrev = dev_->get_software_revision();

        co_return std::pair{std::error_code{}, rsp};
    }

private:
    DeviceController* dev_;
};

// 注册到 Router
secs::protocol::Router router;
auto handler = std::make_shared<MyS1F1Handler>(my_device);
secs::protocol::register_typed_handler(router, 1, 1, handler);

// 或直接注册到 Session 的 Router
secs::protocol::register_typed_handler(proto.router(), 1, 1, handler);
```

#### CEID 分发（可选，不引入 GEM）

对于“消息体携带 CEID”的场景（典型如 `S6F11/S6F12`），可以使用 `secs::protocol::CeidDispatcher` 将：

1. decode body -> `secs::ii::Item`
2. 提取 CEID（按厂商文档定义）
3. 按 CEID 分发到不同 handler

收敛为一个可复用的处理层（不引入 GEM 语义）。

- 头文件：`<secs/protocol/ceid_dispatcher.hpp>`、`<secs/utils/ceid_helpers.hpp>`
- 对应示例：`examples/hsms_custom.cpp`、`examples/secs1_custom.cpp`

```cpp
#include <secs/protocol/ceid_dispatcher.hpp>
#include <secs/utils/ceid_helpers.hpp>

auto disp = std::make_shared<secs::protocol::CeidDispatcher>(
    [](const secs::protocol::DataMessage&, const secs::ii::Item& body)
        -> std::optional<std::uint32_t> {
        return secs::utils::extract_ceid_s6f11_like(body);
    });

disp->set_item(0x1001, [](std::uint32_t ceid, const secs::ii::Item& req_body,
                          const secs::protocol::DataMessage&) -> asio::awaitable<
                              std::pair<std::error_code, secs::ii::Item>> {
    (void)ceid;
    (void)req_body;
    co_return std::pair{std::error_code{}, secs::ii::Item::list({})};
});

secs::protocol::register_ceid_dispatcher(proto.router(), 6, 11, disp);
```

### 3. 主动发送消息

#### 发送不需要回复的消息（W=0）

```cpp
#include <secs/utils/protocol_helpers.hpp>

S6F11Event event;
event.dataid = 0;
event.ceid = 1001;
event.reports = {...};

auto ec = co_await secs::utils::async_send_item(
    proto, 6, 11, secs::ii::to_item(event));
if (ec) {
    // 处理发送错误
}
```

#### 发送请求并等待回复（W=1）

```cpp
S2F13Request req;
req.ecids = {100, 200, 300};

auto [ec, out] = co_await secs::utils::async_request_decoded(
    proto, 2, 13, secs::ii::to_item(req), 5s);

if (ec) {
    // 处理请求错误
} else if (!out.decoded.has_value()) {
    // 合法场景：对端回复空 body
} else {
    // 解析回复
    auto rsp = secs::ii::from_item<S2F14Response>(out.decoded->item);
    if (rsp) {
        // 使用 rsp->ecvs
    }
}
```

---

## 路线二：SMLX 模式 (Rule-Based)

通过配置文件定义消息和行为，几乎不需要编写 C++ 逻辑代码。

### 1. SML 语法速查

文件扩展名通常为 `.sml`。

| 语法 | 说明 |
| :--- | :--- |
| `name: SxFy [W] <Item>.` | 定义消息模板。`W` 表示 W-bit=1。 |
| `<L>` / `<L <U4 1>>` | List 定义。 |
| `<A "Text">` | ASCII 字符串。 |
| `<U4 $VAR>` | **SMLX 变量占位符**。需在运行时注入。 |
| `if (msg_name) rsp_name.` | **自动回复规则**。收到 msg_name 自动回 rsp_name。 |
| `every 10 send msg_name.` | **定时任务**。每 10 秒发送一次。 |

**示例 (equipment.sml):**

```sml
// 定义模板
s1f1: S1F1 W <L>.
s1f2: S1F2 <L <A $MDLN> <A $SOFTREV>>.

// 规则
if (s1f1) s1f2.
```

### 2. 加载与集成

```cpp
#include <secs/sml/runtime.hpp>

secs::sml::Runtime rt;

// 1. 加载文件
std::string sml_content = read_file("equipment.sml");
if (auto ec = rt.load(sml_content); ec) {
    // 处理加载错误
    std::cerr << "SML load failed: " << ec.message() << "\n";
    return;
}

// 2. 集成方式（推荐）：在 Router 上注册一个“规则驱动”的 handler
// - 典型场景：S6F11(W=1) -> S6F12（按 CEID/参数分支）
// - 参考实现：examples/hsms_smlx.cpp、examples/secs1_smlx.cpp
//
// 关键点：
// - match_response_with_capture：按条件规则匹配响应模板，并捕获 $DATAID/$PARAMS 等
// - 在捕获上下文上继续注入业务变量（DEVICE_NAME、温度、报警…）
// - encode_message_body：渲染并编码响应模板，返回可直接用于回包的 body bytes
```

### 3. 变量注入（RenderContext）

SML 模板中的占位符（如 `$MDLN`）需要通过 `RenderContext` 注入值。

#### 场景 A：主动发送消息时注入变量

```cpp
#include <secs/sml/render.hpp>

secs::sml::RenderContext ctx;
ctx.set("MDLN", secs::ii::Item::ascii("MyDevice"));
ctx.set("SOFTREV", secs::ii::Item::ascii("1.0.0"));

// 渲染并编码消息
std::vector<secs::core::byte> body;
std::uint8_t stream, function;
bool w_bit;

auto ec = rt.encode_message_body("s1f2", ctx, body, &stream, &function, &w_bit);
if (ec) {
    std::cerr << "Render failed: " << ec.message() << "\n";
    return;
}

// 发送
if (w_bit) {
    co_await proto.async_request(stream, function,
        secs::core::bytes_view{body.data(), body.size()}, 5s);
} else {
    co_await proto.async_send(stream, function,
        secs::core::bytes_view{body.data(), body.size()});
}
```

#### 场景 B：在 Handler 中动态注入变量

```cpp
// 在自定义 Handler 中
proto.router().set(1, 1,
    [&rt](const secs::protocol::DataMessage& req)
        -> asio::awaitable<secs::protocol::HandlerResult> {

    // 准备变量
    secs::sml::RenderContext ctx;
    ctx.set("MDLN", secs::ii::Item::ascii(get_device_model()));
    ctx.set("SOFTREV", secs::ii::Item::ascii(get_software_version()));

    // 渲染响应
    std::vector<secs::core::byte> body;
    auto ec = rt.encode_message_body("s1f2", ctx, body);
    if (ec) {
        co_return secs::protocol::HandlerResult{ec, {}};
    }

    co_return secs::protocol::HandlerResult{std::error_code{}, std::move(body)};
});
```

### 4. 实现自动回复 Handler

参考实现：`examples/hsms_smlx.cpp`（HSMS）与 `examples/secs1_smlx.cpp`（SECS-I）。

```cpp
proto.router().set(6, 11,
    [&rt, &device](const secs::protocol::DataMessage& req)
        -> asio::awaitable<secs::protocol::HandlerResult> {

        if (!req.w_bit) {
            co_return secs::protocol::HandlerResult{std::error_code{}, {}};
        }

        // 1) decode body -> Item（示例中要求 fully_consumed）
        auto [dec_ec, decoded] = secs::utils::decode_one_item(
            secs::core::bytes_view{req.body.data(), req.body.size()});
        if (dec_ec || !decoded.fully_consumed) {
            co_return secs::protocol::HandlerResult{
                secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
        }

        // 2) 匹配并捕获（capture 写入 ctx）
        secs::sml::RenderContext ctx;
        const auto response_name =
            rt.match_response_with_capture(req.stream, req.function, decoded.item, ctx);
        if (!response_name.has_value()) {
            co_return secs::protocol::HandlerResult{
                secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
        }

        // 3) 在 ctx 上继续注入业务变量（示意）
        fill_context_for_response(*response_name, device, ctx);

        // 4) 渲染并编码响应模板（得到 body bytes）
        std::vector<secs::core::byte> rsp_body;
        std::uint8_t rsp_stream = 0;
        std::uint8_t rsp_function = 0;
        bool rsp_w = false;
        auto enc_ec = rt.encode_message_body(
            *response_name, ctx, rsp_body, &rsp_stream, &rsp_function, &rsp_w);
        if (enc_ec) {
            co_return secs::protocol::HandlerResult{enc_ec, {}};
        }

        // 5) 防呆：确保返回的是 “SxF(y+1) 且 W=0”
        const auto expected_function =
            static_cast<std::uint8_t>(req.function + 1u);
        if (rsp_stream != req.stream || rsp_function != expected_function || rsp_w) {
            co_return secs::protocol::HandlerResult{
                secs::core::make_error_code(secs::core::errc::invalid_argument), {}};
        }

        co_return secs::protocol::HandlerResult{std::error_code{}, std::move(rsp_body)};
    });
```

---

## 错误处理

所有 API 使用 `std::error_code` 返回错误。

```cpp
auto ec = co_await proto.async_send(1, 1, body);
if (ec) {
    std::cerr << "Send failed: " << ec.message()
              << " (category: " << ec.category().name() << ")\n";
}
```

常见错误类别：
- `secs.core`：核心错误（如 `invalid_argument`, `out_of_memory`）
- `secs.ii`：SECS-II 编解码错误
- `sml.render`：SMLX 渲染错误（如 `missing_variable`, `type_mismatch`）
- `sml.lexer` / `sml.parser`：SML 语法错误

---

## 调试与排查（Dump）

开启 Dump 可以打印收发的字节流和解码后的 SECS-II 树结构。

```cpp
secs::protocol::SessionOptions opt;
opt.dump.enable = true;
opt.dump.dump_tx = true;              // 打印发送的字节流
opt.dump.dump_rx = true;              // 打印接收的字节流
opt.dump.dump_secs2_decode = true;    // 打印解码后的 SML 树结构（推荐）
```

---

## 完整示例

主示例集合（推荐，目录顶层）：

- HSMS：`examples/hsms_custom.cpp`、`examples/hsms_smlx.cpp`
- SECS-I：`examples/secs1_custom.cpp`、`examples/secs1_smlx.cpp`
- SML 模板：`examples/ceid_demo.sml`

深入示例（已归档到 legacy，便于参考更细的模式与实验性功能）：

- TypedHandler：`examples/legacy/typed_handler_example.cpp`
- SMLX 主动发送：`examples/legacy/smlx_active_send_example.cpp`
- 自定义回复逻辑：`examples/legacy/protocol_custom_reply_example.cpp`
