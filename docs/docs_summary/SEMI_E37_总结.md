# SEMI E37 - 高速 SECS 消息服务 (HSMS) 标准总结

## 文档概述

SEMI E37 标准定义了半导体工厂中计算机之间交换消息的高速通信接口，基于 TCP/IP 协议，作为 SECS-I (RS-232) 的高速替代方案。

## 一、HSMS 通用服务 (E37-0298)

### 1.1 核心目的

- 提供独立制造商之间的互操作通信标准
- 作为 SECS-I 的替代方案，适用于需要更高速通信的场景
- 作为 SEMI E13 的替代方案，当优先选择 TCP/IP 而非 OSI 时

### 1.2 关键术语定义

| 术语 | 定义 |
|------|------|
| **Entity（实体）** | 与 TCP/IP 连接端点关联的应用程序 |
| **Session（会话）** | 两个实体之间建立的用于交换 HSMS 消息的关系 |
| **Connection（连接）** | TCP/IP LAN 上两个实体之间建立的逻辑链接 |
| **Session ID** | 标识特定会话实体之间特定会话的 16 位无符号整数 |
| **Data Message（数据消息）** | 在 HSMS 会话中传递应用特定数据的消息 |
| **Control Message（控制消息）** | 用于管理 HSMS 会话的消息 |

### 1.3 状态机

HSMS 定义了三个主要状态：

```
NOT CONNECTED → CONNECTED → NOT SELECTED → SELECTED
                    ↑              ↓
                    └──────────────┘
```

**状态描述：**

1. **NOT CONNECTED**: 实体准备监听或发起 TCP/IP 连接
2. **CONNECTED**: TCP/IP 连接已建立
   - **NOT SELECTED**: 尚未建立 HSMS 会话
   - **SELECTED**: 已建立 HSMS 会话（正常工作状态）

### 1.4 TCP/IP 使用

#### 连接模式

- **Passive Mode（被动模式）**: 监听并接受远程实体发起的连接
- **Active Mode（主动模式）**: 向远程实体发起连接

#### 网络寻址

- **IP Address**: 每个物理 TCP/IP 连接必须有唯一的 IP 地址（例如：192.9.200.1）
- **TCP Port Number**: 可视为 IP 地址的扩展（例如：5000）

### 1.5 核心通信过程

#### Select（选择）过程

用于建立 HSMS 通信：

1. 发起方发送 `Select.req` 消息
2. 响应方返回 `Select.rsp`，状态码：
   - 0: 通信已建立（成功）
   - 1: 通信已激活（已存在会话）
   - 2: 连接未就绪
   - 3: 连接耗尽

#### Data（数据）过程

仅在 SELECTED 状态下允许交换数据消息：

- **Primary Message**: 奇数函数代码，事务的第一条消息
- **Reply Message**: 偶数函数代码，对 Primary 的响应

#### Deselect（取消选择）过程

优雅地结束 HSMS 通信：

1. 发起方发送 `Deselect.req`
2. 响应方返回 `Deselect.rsp`

#### Separate（分离）过程

立即终止 HSMS 通信（单向，无响应）

#### Linktest（链路测试）过程

验证连接完整性：

1. 发起方发送 `Linktest.req`
2. 响应方返回 `Linktest.rsp`
3. 如果 T6 超时前未收到响应，视为通信失败

#### Reject（拒绝）过程

响应在不适当上下文中收到的消息

### 1.6 消息格式

#### 通用消息结构

```
[4 字节 Message Length] [10 字节 Header] [0-n 字节 Message Text]
```

#### 消息头格式（10 字节）

| 字节 | 字段 | 说明 |
|------|------|------|
| 0-1 | Session ID | 16 位会话标识符 |
| 2 | Header Byte 2 | 对于数据消息：W-Bit + SECS Stream |
| 3 | Header Byte 3 | 对于数据消息：SECS Function |
| 4 | PType | 表示类型（0 = SECS-II） |
| 5 | SType | 会话类型（0 = 数据消息） |
| 6-9 | System Bytes | 唯一标识事务 |

#### SType 值

| 值 | 消息类型 |
|----|---------|
| 0 | Data Message |
| 1 | Select.req |
| 2 | Select.rsp |
| 3 | Deselect.req |
| 4 | Deselect.rsp |
| 5 | Linktest.req |
| 6 | Linktest.rsp |
| 7 | Reject.req |
| 9 | Separate.req |

### 1.7 关键超时参数

| 参数 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| **T3** | 45秒 | 1-120秒 | 回复超时 - 等待回复消息的最长时间 |
| **T5** | 10秒 | 1-240秒 | 连接分离超时 - 连续连接尝试之间的最短时间 |
| **T6** | 5秒 | 1-240秒 | 控制事务超时 - 控制事务保持打开的最长时间 |
| **T7** | 10秒 | 1-240秒 | NOT SELECTED 超时 - 进入 NOT SELECTED 后的最长等待时间 |
| **T8** | 5秒 | 1-120秒 | 网络字符间超时 - 单个消息连续字节之间的最长时间 |

---

## 二、HSMS 单会话模式 (E37.1-96E)

### 2.1 目的

HSMS-SS 是 SECS-I 的直接替代方案，简化了 HSMS 通用服务的复杂性。

### 2.2 主要限制

1. **消除 Deselect**: 使用 Separate 代替 Deselect 结束通信
2. **Reject 可选**: 不支持 Reject 的实现应将需要 Reject 的情况视为通信失败
3. **简化 Select**:
   - 仅主动连接方可发起 Select
   - 仅在 NOT SELECTED 状态允许
   - 使用 SessionID = 0xFFFF

### 2.3 状态转换差异

HSMS-SS 相比通用 HSMS 的关键差异：

- 不需要 SelectionCounter
- T3 超时不导致连接终止，仅取消数据事务
- T7 超时后必须断开 TCP/IP 连接

### 2.4 Device ID

在 HSMS-SS 数据消息中：

- Session ID 的高位为 0
- 低 15 位包含 Device ID
- Device ID 标识设备内的逻辑子实体

### 2.5 多块消息兼容性

为保持与 SECS-I 的兼容性：

- 单块消息：消息长度 ≤ 254 字节（10 字节头 + 244 字节文本）
- 多块消息：无此限制

---

## 三、HSMS 通用会话模式 (E37.2-95)

### 3.1 目的

支持包含多个独立子系统的复杂系统（如集群工具、轨道系统）。

### 3.2 核心概念

#### Session Entity（会话实体）

- 系统内可单独访问的子实体
- 每个实体有唯一的 Session Entity ID（16位）

#### Session Entity List

- 与特定 IP 地址和端口关联的所有可用会话实体列表

#### Selected Entity List

- 给定 TCP/IP 连接上当前已选择访问的会话实体列表

#### Selection Count

- Selected Entity List 中的实体数量
- 仅当 Selection Count = 0 时才能从 SELECTED 转换到 NOT SELECTED

### 3.3 状态机扩展

HSMS-GS 增加了两个新的状态转换：

| # | 触发 | 操作 | 结果 |
|---|------|------|------|
| 6 | 在 Selection Count > 0 时成功 Select | 递增 Selection Count，添加到 Selected Entity List | 保持 SELECTED |
| 7 | 在 Selection Count > 1 时成功 Deselect/Separate | 递减 Selection Count，从 Selected Entity List 移除 | 保持 SELECTED 或转到 NOT SELECTED（如果 Count = 0） |

### 3.4 Select/Deselect 状态码扩展

| 值 | 说明 |
|----|------|
| 4 | 无此实体 - Session ID 不对应任何可用的 Session Entity |
| 5 | 实体正在使用（被其他连接） |
| 6 | 实体已选择（被当前连接） |

### 3.5 与 HSMS-SS 同时支持

可通过简单测试实现同时支持两种模式：

- 如果第一个 Select.req 的 SessionID = -1（0xFFFF），则作为 HSMS-SS 运行
- 否则作为 HSMS-GS 运行

---

## 四、实现要点

### 4.1 通信失败处理

检测到通信失败时：

1. 终止 TCP/IP 连接
2. 可尝试重新建立通信
3. 设备应发送 SECS-II S9F9（事务超时消息）

### 4.2 消息发送/接收

使用 TCP/IP 流：

- **发送**: 先发送 4 字节长度 + 10 字节头，然后发送消息文本
- **接收**: 先读取长度和头，再根据长度读取文本

### 4.3 Stream 9 消息

SECS-II Stream 9 错误消息中的 MHEAD/SHEAD 应包含 10 字节的 HSMS 消息头（而非 SECS-I 块头）。

### 4.4 必需文档

实现必须记录：

1. 参数设置方法
2. 各参数的允许范围和分辨率
3. 拒绝连接请求的选项（被动模式）
4. 最大可接收消息大小
5. 最大预期发送消息大小
6. 支持的最大并发打开事务数

---

## 五、SECS-I 与 HSMS 对比

| 特性 | SECS-I | HSMS |
|------|--------|------|
| **通信协议** | RS-232 | TCP/IP |
| **物理层** | 25针连接器 + 4线串行电缆 | 不定义（通常为以太网） |
| **通信速度** | ~1000 字节/秒（9600波特） | ~10 Mbits/秒（以太网） |
| **连接** | 每个 SECS-I 连接一根物理电缆 | 一根物理网线支持多个 HSMS 连接 |
| **消息传输** | 分块传输（~256字节/块） | 整个消息作为 TCP/IP 流 |
| **消息头** | 每块10字节头（含 E-Bit、块号） | 整个消息一个10字节头 |
| **最大消息** | ~7.9 MB | ~4 GB |
| **协议参数** | T1, T2, T4, RTY, Baud Rate | T5, T6, T7, T8, IP Address |

---

## 六、典型应用场景

### 场景 1：简单设备连接（HSMS-SS）

```
主机（Active Mode）----- TCP/IP -----设备（Passive Mode）
         |                                    |
    发起连接                             监听连接
    发送 Select.req                   接受并响应
    交换 SECS-II 消息                  处理消息
    发送 Separate.req                  终止会话
```

### 场景 2：集群工具（HSMS-GS）

```
主机 ---- TCP/IP ---- 集群工具
                       |
                       +-- Session Entity 1（装载站）
                       +-- Session Entity 2（反应腔 A）
                       +-- Session Entity 3（反应腔 B）
                       +-- Session Entity 4（卸载站）

主机可以：
- 同时或分别选择多个实体
- 向每个实体发送特定消息
- 独立管理每个实体的会话
```

---

## 七、编程实现建议

### 7.1 连接建立流程（被动模式）

```c
// TLI API 示例
tep = t_open(...);          // 获取连接端点
t_bind(tep, ...);           // 绑定到发布端口
t_listen(tep, ...);         // 监听连接请求
t_accept(tep, ...);         // 接受连接
```

### 7.2 连接建立流程（主动模式）

```c
// TLI API 示例
tep = t_open(...);          // 获取连接端点
t_bind(tep, ...);           // 绑定到空地址
t_connect(tep, ...);        // 发起连接
t_rcvconnect(tep, ...);     // 接收接受确认
```

### 7.3 消息发送

```c
// 设置消息长度
hdr->Len = length;

// 发送头部（14字节 = 4字节长度 + 10字节头）
t_snd(tep, hdr, 14, 0);

// 发送消息文本
t_snd(tep, Text, hdr->Len, 0);
```

### 7.4 消息接收

```c
// 接收头部
t_rcv(tep, hdr, 14, ...);

// 接收消息文本
t_rcv(tep, Text, hdr->Len, ...);
```

---

## 八、关键注意事项

### 8.1 性能优化

- 使用 T5 防止过度连接活动
- T8 检测网络延迟问题
- 支持多个并发事务以提高吞吐量

### 8.2 错误处理

- T3 超时：取消事务，设备发送 S9F9
- T6 超时：通信失败，断开连接
- T7 超时：断开未使用的连接
- T8 超时：检测到网络问题，断开连接

### 8.3 兼容性

- 支持 SECS-I 单块消息限制（254字节）
- Stream 9 消息使用 HSMS 消息头
- 可同时支持 HSMS-SS 和 HSMS-GS

---

## 九、快速参考

### 9.1 状态转换检查清单

✅ **进入 SELECTED 状态前**
- TCP/IP 连接已建立
- Select 过程成功完成

✅ **发送数据消息前**
- 当前状态为 SELECTED
- SessionID 在 Selected Entity List 中（HSMS-GS）

✅ **断开连接前**
- 发送 Separate.req 或完成 Deselect
- 转换到 NOT SELECTED 状态

### 9.2 消息构造检查清单

✅ **所有消息**
- Message Length 正确（头+文本）
- System Bytes 唯一（对于请求消息）

✅ **数据消息**
- W-Bit 正确设置（1 = 需要回复）
- Stream/Function 符合 SECS-II 规范
- SessionID 有效

✅ **控制消息**
- 正确的 SType 值
- 适当的状态下发送

---

## 十、总结

HSMS 标准提供了一个强大、灵活的框架用于半导体设备通信：

1. **HSMS Generic Services**: 定义核心协议和消息格式
2. **HSMS-SS**: 提供简单的 SECS-I 替代方案
3. **HSMS-GS**: 支持复杂多子系统设备

选择合适的模式取决于应用需求：

- **简单点对点通信** → HSMS-SS
- **集群工具、轨道系统** → HSMS-GS
- **需要灵活性** → HSMS Generic Services

实现时应重点关注状态机管理、超时处理和消息格式正确性。
