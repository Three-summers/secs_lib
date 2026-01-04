# SECS/GEM 技术参考文档（面向 secs_lib 项目集成）

生成日期：2026-01-04  
基于：SEMI E4/E5/E37 标准文档分析  
对照实现：secs_lib (C++20)

---

## 文档导航

本技术参考由 6 个专项分析文档组成，按协议层次组织：

### 1. 传输层

#### **SECS-I（串口半双工）** → `analysis_e04_secs1.md`
- 握手协议：ENQ/EOT/ACK/NAK
- Block 帧结构：Header(10B) + Data + Checksum(2B)
- 半双工状态机与重传机制
- 定时器：T1（字符间超时）、T2（协议超时）
- R-bit方向位定义
- **对照实现**：`include/secs/secs1/*.hpp`

#### **HSMS（TCP/IP 全双工）** → `analysis_e37_hsms.md`
- 消息帧结构：Length(4B) + Header(10B) + Body
- 会话状态机：disconnected/connected/selected
- 控制消息：SELECT/DESELECT/LINKTEST/SEPARATE
- 定时器：T3/T5/T6/T7/T8
- SystemBytes 分配与事务匹配
- **对照实现**：`include/secs/hsms/*.hpp`

### 2. 消息内容层（SECS-II）

#### **SECS-II 数据类型与编码规范** → `analysis_e05_secs2.md`
- 数据类型体系：List/Binary/Boolean/ASCII/I*/U*/F*
- Item 编码格式：FormatByte + Length + Value
- 嵌套 List 规则
- Stream/Function 消息标识
- W-bit 与 primary/secondary 响应机制
- **对照实现**：`include/secs/ii/*.hpp`

#### **消息格式字节级编码** → `analysis_message_format.md`
- FormatByte 位域布局
- Length 字段变长编码（1/2/3字节）
- 各数据类型 on-wire 编码示例
- 边界情况：空数组、最大长度、嵌套深度
- **对照实现**：`src/ii/codec.cpp`

#### **SECS-II 核心概念入门** → `analysis_secs2_intro.md`
- Stream/Function/Item/List 概念解释
- 消息结构与语义规则
- 历史背景与设计目标
- 与传输层关系
- **对照实现**：README.md 架构说明

### 3. 应用层（GEM）

#### **GEM 标准模型与集成指导** → `analysis_gem_model.md`
- 通信状态机模型
- Host vs Equipment 角色定义
- 数据采集：事件报告与变量
- 报警处理机制
- 变量/事件/报告标识符体系
- **对照实现**：`include/secs/protocol/*.hpp`

---

## 协议层次关系图

```
┌─────────────────────────────────────────────────┐
│           GEM 应用层（SEMI E30）                │
│  - 设备状态模型                                   │
│  - 事件/报警/Trace 机制                          │
│  - Host/Equipment 交互场景                      │
│  对应文档：analysis_gem_model.md                │
└──────────────▲──────────────────────────────────┘
               │
┌──────────────┴──────────────────────────────────┐
│        SECS-II 消息内容层（SEMI E5）            │
│  - Stream/Function 消息标识                    │
│  - Item/List 数据结构                          │
│  - 编解码规则                                    │
│  对应文档：analysis_e05_secs2.md               │
│           analysis_message_format.md            │
│           analysis_secs2_intro.md               │
└──────────────▲──────────────────────────────────┘
               │
      ┌────────┴────────┐
      │                 │
┌─────┴───────┐  ┌──────┴────────┐
│ SECS-I (E4) │  │  HSMS (E37)   │
│ 串口半双工    │  │  TCP/IP 全双工│
│ analysis_   │  │  analysis_    │
│ e04_secs1   │  │  e37_hsms     │
└─────────────┘  └───────────────┘
```

---

## 快速查找：关键算法伪代码位置

| 主题 | 文档位置 |
|------|---------|
| SECS-I 握手流程 | `analysis_e04_secs1.md` § 4.2 |
| SECS-I 分包/重组 | `analysis_e04_secs1.md` § 4.4 |
| SECS-I 校验和计算 | `analysis_e04_secs1.md` § 4.1 |
| HSMS SELECT 握手 | `analysis_e37_hsms.md` § 3.1 |
| HSMS 事务匹配 | `analysis_e37_hsms.md` § 5.2 |
| HSMS Framing | `analysis_e37_hsms.md` § 1.2 |
| SECS-II Item 序列化 | `analysis_e05_secs2.md` § 5.1 |
| SECS-II Item 反序列化 | `analysis_e05_secs2.md` § 5.2 |
| FormatByte 编码 | `analysis_message_format.md` § 6.1 |
| Length 变长编码 | `analysis_message_format.md` § 6.1 |

---

## 与 secs_lib 实现对照索引

### 完全符合标准的部分

| 标准要求 | 项目实现 |
|---------|---------|
| SECS-I 握手字节 | `include/secs/secs1/block.hpp` |
| SECS-I Block 最大长度=254（Data=244） | `include/secs/secs1/block.hpp` |
| SECS-I Block Number 15-bit | `include/secs/secs1/block.hpp`、`src/secs1/block.cpp` |
| SECS-I 重复块检测（ACK 丢失重传容忍） | `src/secs1/state_machine.cpp` |
| SECS-I Checksum | `src/secs1/block.cpp` |
| HSMS Framing | `src/hsms/message.cpp` |
| HSMS Select/Deselect.rsp 状态码字段 | `include/secs/hsms/message.hpp`、`src/hsms/message.cpp` |
| HSMS-SS 控制消息 SessionID=0xFFFF | `src/hsms/session.cpp` |
| HSMS T8 超时 | `src/hsms/connection.cpp` |
| SECS-II FormatByte | `src/ii/codec.cpp` |
| SECS-II Length 编码 | `src/ii/codec.cpp` |
| SystemBytes 分配 | `include/secs/protocol/system_bytes.hpp` |

### 已修正的历史差异（现已对齐标准）

本项目早期实现曾为兼容既有参考实现（如 c_dump）而做过若干取舍；截至 2026-01-04，
本节列出的差异已完成修正，并已在单元测试中固化行为。

---

## 建议的集成检查清单

### 传输层集成

- [ ] HSMS 会话状态门控（只在 `selected` 发送数据消息）
- [ ] T3/T6/T7/T8 定时器配置
- [ ] SystemBytes 分配/回收策略
- [ ] 断线重连与状态恢复

### 消息内容层集成

- [ ] Item 编解码资源限制（DecodeLimits）
- [ ] 强类型消息映射（TypedHandler）
- [ ] Stream/Function 路由注册

### 应用层集成

- [ ] S1F13/S1F14 通信建立流程
- [ ] S2F33/S2F35/S2F37 数据采集配置
- [ ] S5F1/S5F2 报警处理
- [ ] S6F11/S6F12 事件报告

---

## 文档版本

| 版本 | 日期 | 变更内容 |
|------|------|---------|
| 1.0 | 2026-01-04 | 初始版本，基于 SEMI E4-0699/E5-0200/E37-0999 分析 |
| 1.1 | 2026-01-04 | 对齐实现：修正 SECS-I（244B/15-bit/重复块）与 HSMS-SS（控制消息字段/SessionID=0xFFFF）差异点 |

---

## 相关资源

- secs_lib 项目 README：`../secs_lib/README.md`
- 标准文档原文：`SEMI_E04_-_0699.pdf`、`SEMI+E05+-+0200.pdf`、`SEMI_E37_-_0999.pdf`
- 项目代码：`../secs_lib/include/secs/`、`../secs_lib/src/`
