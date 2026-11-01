/* WiFi STA：连接手机热点，带指数退避重连
 * 依赖：esp_wifi / nvs_flash / esp_event / esp_netif
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief 启动 STA 并等待首次连上（阻塞）
 *
 * 连上后事件回调会自动维持连接：断开时按 1/2/4/8/8... 秒退避重连，永不放弃。
 *
 * @param ssid      热点名
 * @param pass      密码（开放热点传 NULL 或 ""）
 * @param timeout_ms 等待首次连上的超时；0 = 立即返回（后台连）
 * @return ESP_OK 连上；ESP_ERR_TIMEOUT 超时但仍在后台重试
 */
esp_err_t wifi_sta_start(const char *ssid, const char *pass, int timeout_ms);

bool      wifi_sta_is_connected(void);

/** 取 IPv4 字符串，未连上返回 ESP_ERR_INVALID_STATE */
esp_err_t wifi_sta_get_ip(char *buf, size_t len);

/** 当前 RSSI（dBm），未连上返回 0 */
int       wifi_sta_get_rssi(void);

/** 已重连次数（用于观测链路稳定性） */
int       wifi_sta_get_retry_count(void);

/**
 * @brief 扫描周围 AP 并打印（2.4 GHz），用于诊断"找不到热点"
 *
 * @param max_ap 最多打印多少个，0 = 全部
 * @return 扫到的 AP 数量；<0 表示失败
 */
int       wifi_sta_scan_dump(int max_ap);
