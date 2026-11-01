#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "wifi_sta.h"

static const char *TAG = "WIFI";

#define BIT_GOT_IP  BIT0
#define BIT_FAIL    BIT1

static EventGroupHandle_t s_eg;
static esp_netif_t       *s_netif;
static esp_timer_handle_t s_reconnect_timer;

static volatile bool s_connected;
static volatile int  s_retry;          /* 当前退避档位 */
static volatile int  s_retry_total;    /* 累计重连次数 */
static char          s_ip[20];
static char          s_ssid[33];
static char          s_pass[65];

/* ---- 退避重连：1/2/4/8/8/8... 秒 ---- */
static void reconnect_cb(void *arg)
{
    (void)arg;
    if (!s_connected) {
        ESP_LOGW(TAG, "重连中（退避档 %d）...", s_retry);
        esp_wifi_connect();
    }
}

/* 退避表：前几次快速重试（首次连接常因信道扫描/认证超时而失败），
 * 之后才拉开间隔，避免长时间断开时频繁唤醒。 */
static const uint32_t k_backoff_ms[] = { 200, 400, 800, 2000, 4000, 8000 };

static uint32_t next_backoff_ms(void)
{
    int idx = s_retry;
    if (idx >= (int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]))) {
        idx = (int)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])) - 1;
    } else {
        s_retry++;
    }
    s_retry_total++;
    return k_backoff_ms[idx];
}

static void schedule_reconnect(void)
{
    uint32_t ms = next_backoff_ms();
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)ms * 1000ULL);
}

/* ---- 事件 ---- */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ip[0] = '\0';
        uint32_t ms = next_backoff_ms();
        ESP_LOGW(TAG, "断开 (reason=%d)，%u ms 后重试", d ? d->reason : -1, (unsigned)ms);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)ms * 1000ULL);
        return;
        schedule_reconnect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        wifi_ap_record_t ap;
        int ch = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.primary : -1;
        ESP_LOGI(TAG, "已连接  SSID=%s  IP=%s  ch=%d", s_ssid, s_ip, ch);
        xEventGroupSetBits(s_eg, BIT_GOT_IP);
    }
}

esp_err_t wifi_sta_start(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (pass) {
        strncpy(s_pass, pass, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_eg = xEventGroupCreate();
    if (!s_eg) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_FAIL;

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_ip_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    if (s_pass[0]) {
        strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 限制发射功率，降低峰值电流（F2 为 0.5 A PTC） */
    esp_wifi_set_max_tx_power(52);   /* 单位 0.25 dBm → 13 dBm */

    /* 关键：关闭 modem sleep。默认省电模式会让每个数据包等下一个 DTIM 窗口，
       实测 ICMP RTT 平均 197 ms、HTTP 每请求 ~190 ms（连 327 字节的接口都这么慢），
       预览只能跑到 5~7 fps。关掉后 RTT 降到个位数 ms。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_timer_create_args_t ta = {
        .callback = reconnect_cb,
        .name = "wifi_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_reconnect_timer));

    ESP_LOGI(TAG, "开始连接 SSID=%s ...", s_ssid);

    if (timeout_ms <= 0) return ESP_OK;

    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) return ESP_OK;

    ESP_LOGE(TAG, "连接超时（%d ms），后台继续重试", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_sta_get_ip(char *buf, size_t len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;
    if (!s_connected || !s_ip[0]) return ESP_ERR_INVALID_STATE;
    strncpy(buf, s_ip, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}

int wifi_sta_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected) return 0;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

int wifi_sta_get_retry_count(void)
{
    return s_retry_total;
}

int wifi_sta_scan_dump(int max_ap)
{
    /* 扫描与连接互斥：先断开，扫完由退避定时器重新连接 */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* 阻塞扫描 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        ESP_LOGW(TAG, "扫描到 0 个 AP（热点可能未开启，或为 5 GHz）");
        return 0;
    }
    if (num > 32) num = 32;

    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) return -1;
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    ESP_LOGI(TAG, "扫描到 %d 个 AP：", got);
    int show = (max_ap > 0 && max_ap < got) ? max_ap : got;
    for (int i = 0; i < show; i++) {
        ESP_LOGI(TAG, "  [%2d] %-24s ch=%-3d rssi=%4d  auth=%d",
                 i + 1, (char *)recs[i].ssid, recs[i].primary,
                 recs[i].rssi, recs[i].authmode);
    }
    free(recs);
    return got;
}
