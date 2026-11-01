#include "camera_driver.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_camera.h"
#include "sensor.h"

static const char *TAG = "CAMERA";

/* 摄像头主时钟。实测出流期 SCCB 受并行数据线干扰，若降速仍不足可把这里
   改成 10000000（PCLK 随之 5 MHz，耦合源功率减半）再复测。 */
#ifndef CAM_XCLK_HZ
#define CAM_XCLK_HZ  20000000
#endif

static bool s_inited      = false;
static int  s_w           = 0;
static int  s_h           = 0;
static bool s_gray_native = false;
static int  s_pid         = 0;

static framesize_t framesize_from_wh(int w, int h)
{
    if (w == 96  && h == 96)  return FRAMESIZE_96X96;
    if (w == 128 && h == 128) return FRAMESIZE_128X128;
    if (w == 160 && h == 120) return FRAMESIZE_QQVGA;
    if (w == 240 && h == 240) return FRAMESIZE_240X240;
    if (w == 320 && h == 240) return FRAMESIZE_QVGA;
    ESP_LOGW(TAG, "不支持的目标分辨率 %dx%d，回退 96x96", w, h);
    return FRAMESIZE_96X96;
}

static const char *pid_name(int pid)
{
    switch (pid) {
    case 0x26: return "OV2640";
    case 0x56: return "OV5640";
    case 0x36: return "OV3660";
    default:   return "UNKNOWN";
    }
}

/**
 * 中心裁剪 + 最近邻缩放到 CAM_GRAY_W x CAM_GRAY_H。
 * src_bpp = 1 表示 Y8 灰度；src_bpp = 2 表示 YUYV（取偶数下标即 Y）。
 */
static void crop_scale_to_gray(const uint8_t *src, int src_w, int src_h,
                               int src_bpp, uint8_t *dst)
{
    int side = (src_w < src_h) ? src_w : src_h;
    int ox = (src_w - side) / 2;
    int oy = (src_h - side) / 2;

    for (int y = 0; y < CAM_GRAY_H; y++) {
        int sy = oy + (y * side) / CAM_GRAY_H;
        const uint8_t *row = src + (size_t)sy * src_w * src_bpp;
        uint8_t *out_row = dst + (size_t)y * CAM_GRAY_W;
        for (int x = 0; x < CAM_GRAY_W; x++) {
            int sx = ox + (x * side) / CAM_GRAY_W;
            out_row[x] = row[(size_t)sx * src_bpp];
        }
    }
}

esp_err_t camera_driver_init(const camera_driver_cfg_t *cfg)
{
    if (s_inited) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_OK;
    }

    camera_driver_cfg_t c = CAMERA_DRIVER_CFG_DEFAULT;
    if (cfg != NULL) {
        c = *cfg;
        if (c.width <= 0)   c.width = CAM_GRAY_W;
        if (c.height <= 0)  c.height = CAM_GRAY_H;
        if (c.fb_count <= 0) c.fb_count = 3;
    }

    camera_config_t cc = {0};
    cc.ledc_channel = LEDC_CHANNEL_0;
    cc.ledc_timer   = LEDC_TIMER_0;

    cc.pin_pwdn  = CAM_PIN_PWDN;
    cc.pin_reset = CAM_PIN_RESET;
    cc.pin_xclk  = CAM_PIN_XCLK;
    cc.pin_sccb_sda = CAM_PIN_SIOD;
    cc.pin_sccb_scl = CAM_PIN_SIOC;
    cc.pin_d0 = CAM_PIN_D0;
    cc.pin_d1 = CAM_PIN_D1;
    cc.pin_d2 = CAM_PIN_D2;
    cc.pin_d3 = CAM_PIN_D3;
    cc.pin_d4 = CAM_PIN_D4;
    cc.pin_d5 = CAM_PIN_D5;
    cc.pin_d6 = CAM_PIN_D6;
    cc.pin_d7 = CAM_PIN_D7;
    cc.pin_vsync = CAM_PIN_VSYNC;
    cc.pin_href  = CAM_PIN_HREF;
    cc.pin_pclk  = CAM_PIN_PCLK;

    cc.pixel_format = PIXFORMAT_GRAYSCALE;
    cc.frame_size   = framesize_from_wh(c.width, c.height);
    cc.xclk_freq_hz = (c.xclk_hz > 0) ? c.xclk_hz : CAM_XCLK_HZ;
    cc.grab_mode    = CAMERA_GRAB_LATEST;
    cc.fb_location  = c.use_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    cc.fb_count     = (size_t)c.fb_count;
    cc.jpeg_quality = 12;

    esp_err_t err = esp_camera_init(&cc);

    if (err != ESP_OK && c.xclk_hz == 0) {
        ESP_LOGW(TAG, "20 MHz 初始化失败(0x%x)，降频到 10 MHz 重试", err);
        cc.xclk_freq_hz = 10000000;
        err = esp_camera_init(&cc);
    }

    if (err != ESP_OK && cc.frame_size == FRAMESIZE_96X96) {
        ESP_LOGW(TAG, "96x96 不支持(0x%x)，降级为 240x240（grab 时中心裁剪）", err);
        cc.frame_size = FRAMESIZE_240X240;
        err = esp_camera_init(&cc);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "相机初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;

    sensor_t *s = esp_camera_sensor_get();
    s_pid = (s != NULL) ? s->id.PID : -1;
    ESP_LOGI(TAG, "传感器: %s (PID=0x%02X), XCLK=%d Hz", pid_name(s_pid), s_pid, cc.xclk_freq_hz);

    /* 出流后 SCCB 健康检查：只做**读**，无副作用。
       实测表明传感器一开始输出 PCLK，SDA/SCL 就会受并行数据线干扰，
       100 kHz 下约 8%~26% 的事务被 NACK；降到 10 kHz 后应全通过。
       注意：OV2640 的 bank 编码在 bit8，传感器 bank 的 PID 高字节 = 0x010A。 */
    if (s != NULL) {
        for (int i = 0; i < 3; i++) {
            int v = s->get_reg(s, 0x010A, 0xFF);
            if (v == 0x26) {
                ESP_LOGI(TAG, "出流后 SCCB 读 PID: 0x%02X OK（第 %d 次）", v, i + 1);
                break;
            }
            ESP_LOGW(TAG, "出流后 SCCB 读 PID 异常: %d（第 %d 次）", v, i + 1);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* 抓一帧探测真实布局：原生灰度 len == W*H；YUV422 len == 2*W*H */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "探测帧获取失败");
        esp_camera_deinit();
        s_inited = false;
        return ESP_ERR_TIMEOUT;
    }

    s_w = fb->width;
    s_h = fb->height;
    size_t px = (size_t)s_w * s_h;
    s_gray_native = (fb->len == px);

    ESP_LOGI(TAG, "帧: %dx%d, len=%u (期望灰度 %u / YUV422 %u) -> %s",
             s_w, s_h, (unsigned)fb->len, (unsigned)px, (unsigned)(px * 2),
             s_gray_native ? "原生灰度" : "YUV422 取 Y 兜底");

    if (!s_gray_native && fb->len < px * 2) {
        ESP_LOGE(TAG, "帧长度异常，既非 1B/px 也非 2B/px");
        esp_camera_fb_return(fb);
        esp_camera_deinit();
        s_inited = false;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_camera_fb_return(fb);
    return ESP_OK;
}

esp_err_t camera_driver_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    esp_err_t err = esp_camera_deinit();
    s_inited = false;
    s_w = s_h = 0;
    s_gray_native = false;
    return err;
}

esp_err_t camera_driver_info(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "--- 相机信息 ---");
    ESP_LOGI(TAG, "PID      : 0x%02X (%s)", s_pid, pid_name(s_pid));
    ESP_LOGI(TAG, "分辨率   : %dx%d", s_w, s_h);
    ESP_LOGI(TAG, "输出格式 : %s", s_gray_native ? "GRAYSCALE 1B/px" : "YUV422 2B/px");
    ESP_LOGI(TAG, "灰度窗口 : %dx%d = %d B, 时域 %d 帧 = %d B",
             CAM_GRAY_W, CAM_GRAY_H, CAM_GRAY_SIZE, CAM_WIN_FRAMES, CAM_WIN_SIZE);
    return ESP_OK;
}

int camera_driver_width(void)  { return s_w; }
int camera_driver_height(void) { return s_h; }
bool camera_driver_gray_native(void) { return s_gray_native; }

esp_err_t camera_driver_grab_gray(uint8_t *dst, size_t dst_len)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst == NULL || dst_len < CAM_GRAY_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return ESP_ERR_TIMEOUT;
    }

    size_t px = (size_t)fb->width * fb->height;
    if (fb->len == px) {
        crop_scale_to_gray(fb->buf, fb->width, fb->height, 1, dst);
    } else if (fb->len >= px * 2) {
        crop_scale_to_gray(fb->buf, fb->width, fb->height, 2, dst);   /* YUYV，取 Y */
    } else {
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_camera_fb_return(fb);
    return ESP_OK;
}

esp_err_t camera_driver_window_reset(void)
{
    return ESP_OK;   /* 窗口无内部状态：每次 grab 都填满一整窗 */
}

esp_err_t camera_driver_window_grab(uint8_t *out, size_t out_len, float *fps_out)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len < CAM_WIN_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t period_us = 1000000 / CAM_WIN_FPS;
    int64_t t_start = esp_timer_get_time();
    int64_t next = t_start;

    for (int i = 0; i < CAM_WIN_FRAMES; i++) {
        int64_t now = esp_timer_get_time();
        int64_t wait_ms = (next - now) / 1000;
        if (wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
        } else if (wait_ms > -1000) {
            taskYIELD();
        }

        esp_err_t err = camera_driver_grab_gray(out + (size_t)i * CAM_GRAY_SIZE, CAM_GRAY_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "窗口第 %d 帧抓取失败: %s", i, esp_err_to_name(err));
            return err;
        }
        next += period_us;
    }

    int64_t elapsed = esp_timer_get_time() - t_start;
    if (fps_out != NULL) {
        *fps_out = (elapsed > 0)
                 ? (float)(CAM_WIN_FRAMES - 1) * 1000000.0f / (float)elapsed
                 : 0.0f;
    }
    return ESP_OK;
}
