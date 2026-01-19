/*
 * CEID Demo（SMLX）—— 示例专用
 *
 * 目标：
 * - 给 `*_smlx` 示例提供一份“可读、可改、可复用”的 SMLX 模板；
 * - 覆盖两类能力：
 *   1) 条件规则 + Data Capture（$NAME）用于自动选择响应；
 *   2) 响应模板占位符（Identifier）+ RenderContext 变量注入。
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
  >
>.

temperature_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1002>
  <L
    <A "Temperature Sensors">
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
