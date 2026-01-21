/*
 * CEID Demo（SMLX）—— 面向用户的示例模板
 *
 * 目标：
 * - 给 `*_smlx` 示例提供一份“可读、可改、可复用”的 SMLX 模板；
 * - 覆盖两类能力：
 *   1) 条件规则 + Data Capture（$NAME）用于自动选择响应；
 *   2) 响应模板占位符（Identifier）+ RenderContext 变量注入。
 *
 * 本文件额外覆盖当前 SMLX 的全部语法点（便于用户直接对照学习）：
 * - 值占位符（Identifier）：<U2 DATAID> / <A DEVICE_NAME> / <B BYTES> / <Boolean BOOLS> ...
 * - 字符串插值（仅 A "..." 内）：<A "DATAID=${DATAID} STATUS=${STATUS_CODE}">
 *   - 插值变量会自动转为字符串：即使捕获到的是 U2/U4/F4/Binary/Boolean 也可直接插入；
 *   - List 不支持插值（会报 type_mismatch），建议用 <A ...> 单独承载展示字符串。
 * - 占位符与字面量混写：<U2 1 SVIDS 3> / <B 0x01 BYTES 255> / <Boolean 0 BOOLS 1>
 * - （语法展示）== 期望值匹配、every 定时规则：见文件尾部 “SMLX 语法展示”。
 *
 * 约定（与示例代码一致）：
 * - Host/Client 发送 S6F11(W=1)：
 *     <L [3] <U2 DATAID> <U2 CEID> <L ...PARAMS>>
 * - Equipment/Server 返回 S6F12：
 *     <L <U2 DATAID> <U2 CEID> <L ...DATA>>
 *
 * CEID 场景：
 * - 0x1001: 设备状态
 * - 0x1002: 温度数据
 * - 0x1003: 报警信息
 * - 0x1004: 生产统计
 */

/* ===================== 主机侧请求模板（用于主动发送） ===================== */

req_status: S6F11 W
<L
  <U2 DATAID>
  <U2 0x1001>
  <L>
>.

req_temperature: S6F11 W
<L
  <U2 DATAID>
  <U2 0x1002>
  <L>
>.

req_alarm: S6F11 W
<L
  <U2 DATAID>
  <U2 0x1003>
  <L>
>.

req_production: S6F11 W
<L
  <U2 DATAID>
  <U2 0x1004>
  <L>
>.

/* ===================== 设备侧响应模板（用于自动回包） ===================== */

status_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1001>
  <L
    <A DEVICE_NAME>
    <U1 STATUS_CODE>
    <U4 UPTIME_SECONDS>
    <A "INFO name=${DEVICE_NAME} status=${STATUS_CODE} uptime=${UPTIME_SECONDS}s dataid=${DATAID}">
    <A "EXPAND svids=${SVIDS} bytes=${BYTES} bools=${BOOLS}">
    <U2 1 SVIDS 3>
    <B 0x01 BYTES 255>
    <Boolean 0 BOOLS 1>
  >
>.

temperature_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1002>
  <L
    <A "Temperature Sensors">
    <A "AVG=${TEMP_AVG} (t1=${TEMP_SENSOR_1} t2=${TEMP_SENSOR_2} t3=${TEMP_SENSOR_3})">
    <L
      <F4 TEMP_SENSOR_1>
      <F4 TEMP_SENSOR_2>
      <F4 TEMP_SENSOR_3>
    >
    <F4 TEMP_AVG>
  >
>.

alarm_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1003>
  <L
    <U2 ALARM_COUNT>
    <A "COUNT=${ALARM_COUNT} MSG1=${ALARM_MSG_1}">
    <L
      <A ALARM_MSG_1>
      <A ALARM_MSG_2>
    >
  >
>.

production_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1004>
  <L
    <U4 TOTAL_COUNT>
    <U4 GOOD_COUNT>
    <U4 BAD_COUNT>
    <F4 YIELD_RATE>
    <A "TOTAL=${TOTAL_COUNT} GOOD=${GOOD_COUNT} BAD=${BAD_COUNT} YIELD=${YIELD_RATE}">
  >
>.

/* ===================== 条件规则（自动选择响应 + 捕获 DATAID/PARAMS） ===================== */

/* Data Capture：
 * - `$DATAID`：捕获请求中的 DATAID（U2）
 * - `$PARAMS`：捕获 params 子树（List）
 */
if (S6F11 <L [3] <U2 $DATAID> <U2 0x1001> <L $PARAMS>>) status_response.
if (S6F11 <L [3] <U2 $DATAID> <U2 0x1002> <L $PARAMS>>) temperature_response.
if (S6F11 <L [3] <U2 $DATAID> <U2 0x1003> <L $PARAMS>>) alarm_response.
if (S6F11 <L [3] <U2 $DATAID> <U2 0x1004> <L $PARAMS>>) production_response.

/* ===================== SMLX 语法展示（不参与主 CEID 流程） ===================== */

/* 1) 期望值匹配（==<Item>）+ 占位符（Identifier）：
 *
 * 说明：
 * - `==` 的期望值也可以包含占位符（Identifier），匹配时会先用 RenderContext 渲染；
 * - 下面规则仅用于展示语法：当前主示例不会发送 S2F21。
 */

ack_ok: S2F22 <L <U1 0>>.
ack_ng: S2F22 <L <U1 1>>.

/* List 路径索引：S2F21[0] 表示 body 根 List 的第 0 个子元素 */
if (S2F21[0]==<A EXPECT_CMD>) ack_ok.
if (S2F21[0]==<A "PING">) ack_ok.
if (S2F21[0]==<A "">) ack_ng.

/* 2) every 定时规则（语法展示）：
 * - 解析后会出现在 Runtime::timers()；
 * - 注意：本仓库主示例 `hsms_smlx/secs1_smlx` 不会自动执行 timers，
 *   如需周期性发送请参考 `examples/legacy/*_sml_peer*`。
 */
heartbeat: S1F13 W <L <A "HB dataid=${DATAID}">>.
every 30 send heartbeat.

/* 3) 各数据类型 + 占位符/插值/混写（语法全集示意）：
 * - 该消息不参与规则匹配，仅用于展示“模板写法”。
 */
syntax_showcase: 'S1F99' <L
  <A MDLN>
  <A "MDLN=${MDLN} SOFTREV=${SOFTREV}">
  <U2 1 SVIDS 3>
  <B 0x01 BYTES 255>
  <Boolean 0 BOOLS 1>
  <I2 -1 I2VALS 2>
  <F4 0.0 FVALS 100.0>
>.
