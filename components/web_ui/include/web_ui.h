/* web_ui —— 手机网页控制端
 *
 * 职责：
 *   - 提供单文件页面（内嵌 index.html）
 *   - WebSocket 推送 96×96 灰度预览（录制期间自动关闭，见采样时序方案）
 *   - REST：状态 / 录制 / 预览开关 / 回放（数据管理接口在 S4.5 补齐）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** 启动 HTTP 服务（端口 80）。需先初始化 WiFi 与录制器 */
esp_err_t web_ui_start(void);

/** 泵任务每帧调用；仅在预览开启时真正发送 */
void web_ui_push_frame(const uint8_t *gray);

bool web_ui_preview_enabled(void);
void web_ui_set_preview(bool on);

/** 预览帧率上限（1~25），0 = 关闭 */
void web_ui_set_preview_fps(int fps);

/** 当前 WS 客户端数（0/1），用于日志与状态 */
int  web_ui_ws_clients(void);

/** 预览累计发送帧数（用于验证"录制期间图传停止"） */
uint32_t web_ui_preview_sent(void);
