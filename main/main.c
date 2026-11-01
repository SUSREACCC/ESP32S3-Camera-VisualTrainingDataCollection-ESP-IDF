/* 生产版本入口：视觉采集终端
 *
 * 开机流程：SD → 配置 → 索引 → 相机 → 录制器 → WiFi → HTTP 服务 → 取帧泵
 * 不再包含任何自检代码；采集通过网页操作。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "camera_driver.h"
#include "sd_files_driver.h"
#include "sd_index.h"
#include "capture_recorder.h"
#include "cfg_store.h"
#include "wifi_sta.h"
#include "web_ui.h"
#include "wifi_credentials.h"

static const char *TAG = "APP";

/* 取帧泵：钉在 core0，相机节奏驱动；录制与预览都从这一帧分发 */
static void pump_task(void *arg)
{
    (void)arg;
    uint8_t *gray = heap_caps_malloc(REC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!gray) {
        ESP_LOGE(TAG, "帧缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        if (camera_driver_grab_gray(gray, REC_FRAME_BYTES) == ESP_OK) {
            rec_pump_frame(gray);
            if (web_ui_preview_enabled()) web_ui_push_frame(gray);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=========== 视觉采集终端 ===========");

    if (sd_files_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "SD 挂载失败，采集功能不可用");
    }
    cfg_load();
    const cfg_t *cf = cfg_get();
    char crc[12];
    cfg_crc_hex(crc, sizeof(crc));
    ESP_LOGI(TAG, "配置: %s  crc=%s  types=%u  vars=%u",
             cf->from_file ? "config.json" : "内置默认", crc, cf->type_count, cf->var_count);

    if (sd_index_init() != ESP_OK) ESP_LOGE(TAG, "索引初始化失败");

    if (camera_driver_init(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "相机初始化失败");
        return;
    }
    camera_driver_info();

    ESP_ERROR_CHECK(rec_init());

    char ip[20] = "-";
    if (wifi_sta_start(WIFI_SSID, WIFI_PASS, 15000) == ESP_OK) {
        wifi_sta_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, ">>> 手机浏览器打开: http://%s/", ip);
    } else {
        ESP_LOGW(TAG, "WiFi 暂未连上，后台重试中");
    }

    ESP_ERROR_CHECK(web_ui_start());
    xTaskCreatePinnedToCore(pump_task, "pump", 6144, NULL, 4, NULL, 0);

    int n = 0;
    int64_t t0 = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if ((esp_timer_get_time() - t0) / 1000000 >= 15) {
            t0 = esp_timer_get_time();
            rec_progress_t p;
            rec_state(&p);
            char cur[20] = "-";
            wifi_sta_get_ip(cur, sizeof(cur));
            ESP_LOGI(TAG, "[alive %d] wifi=%s(%d) 组=%d next=%u 预览=%u 状态=%d 内部RAM=%u",
                     ++n, cur, wifi_sta_get_rssi(), sd_index_count(),
                     (unsigned)sd_index_next_id(), (unsigned)web_ui_preview_sent(),
                     (int)p.state, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }
}
