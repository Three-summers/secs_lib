/*
 * SML + CEID 完整示例：设备状态查询系统
 *
 * 场景：Equipment 接收 S6F11 请求，根据 CEID 返回不同的 S6F12 响应
 *
 * 消息结构：
 *   S6F11 (W=1): <L <U2 DATAID> <U2 CEID> <L ...params>>
 *   S6F12:       <L <U2 DATAID> <U2 CEID> <L ...data>>
 */

/* ========== 响应模板定义（带占位符） ========== */

/* CEID 0x1001: 设备状态查询
 * 返回：设备名称、状态码、运行时间
 */
status_response: S6F12
<L
  <U2 DATAID>           /* 变量：从请求中提取 */
  <U2 0x1001>           /* CEID 回显 */
  <L
    <A DEVICE_NAME>     /* 变量：设备名称 */
    <U1 STATUS_CODE>    /* 变量：状态码 (0=idle, 1=running, 2=error) */
    <U4 UPTIME_SECONDS> /* 变量：运行时间（秒） */
  >
>.

/* CEID 0x1002: 温度数据查询
 * 返回：多个传感器的温度值
 */
temperature_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1002>
  <L
    <A "Temperature Sensors">
    <L
      <F4 TEMP_SENSOR_1>  /* 变量：传感器1温度 */
      <F4 TEMP_SENSOR_2>  /* 变量：传感器2温度 */
      <F4 TEMP_SENSOR_3>  /* 变量：传感器3温度 */
    >
    <F4 TEMP_AVG>         /* 变量：平均温度 */
  >
>.

/* CEID 0x1003: 报警信息查询
 * 返回：报警列表
 */
alarm_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1003>
  <L
    <U2 ALARM_COUNT>      /* 变量：报警数量 */
    <L
      <A ALARM_MSG_1>     /* 变量：报警消息1 */
      <A ALARM_MSG_2>     /* 变量：报警消息2 */
    >
  >
>.

/* CEID 0x1004: 生产数据查询
 * 返回：生产统计
 */
production_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1004>
  <L
    <U4 TOTAL_COUNT>      /* 变量：总生产数 */
    <U4 GOOD_COUNT>       /* 变量：良品数 */
    <U4 BAD_COUNT>        /* 变量：不良品数 */
    <F4 YIELD_RATE>       /* 变量：良率 */
  >
>.

/* ========== 条件响应规则 ========== */

/* 根据 CEID 选择响应模板
 *
 * 说明：
 * - 条件中的 s6f11 会被自动解析为 stream=6, function=11
 * - 不需要预先定义 s6f11 消息模板
 * - 推荐新语法：[i] 为 0-based 的 List 下标（更直观）
 * - 兼容旧语法：(n) 为 1-based 的先序遍历编号（包含根节点）
 * - (n) 与 [i] 互斥，不能写成 s6f11(3)[1]
 *
 * S6F11 结构: <L <U2 DATAID> <U2 CEID> <L>>
 *
 * 新语法（推荐）：取根 List 的第 i 个子元素（从 0 开始）
 *   [0] = <U2 DATAID>
 *   [1] = <U2 CEID>      <-- CEID 在这里
 *   [2] = <L> params
 *
 * 旧语法（兼容）：先序遍历编号（包含根节点，从 1 开始）
 *   (1) = 根节点 List
 *   (2) = <U2 DATAID>
 *   (3) = <U2 CEID>      <-- 等价于 [1]
 *   (4) = <L> params
 *
 * 期望值占位符：
 * - ==<U2 EXPECTED_CEID_STATUS> 中的 EXPECTED_CEID_STATUS 会在匹配时按 RenderContext 渲染；
 * - 若缺失该变量，规则会“不命中”（可用 match_response_with_trace() 调试）。
 */
if (s6f11[1]==<U2 EXPECTED_CEID_STATUS>) status_response.
if (s6f11[1]==<U2 0x1002>) temperature_response.
if (s6f11[1]==<U2 0x1003>) alarm_response.
if (s6f11[1]==<U2 0x1004>) production_response.

/* 旧语法对照（不参与匹配） */
/* if (s6f11(3)==<U2 0x1001>) status_response. */

/* ========== 可选：定时发送心跳 ========== */

heartbeat: S1F1 W
<L
  <A DEVICE_NAME>
  <A "HEARTBEAT">
>.

/* 每 30 秒发送一次心跳（可选，演示定时规则） */
/* every 30 send heartbeat. */
