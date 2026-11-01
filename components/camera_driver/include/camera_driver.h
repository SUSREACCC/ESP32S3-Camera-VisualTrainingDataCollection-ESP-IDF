#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 灰度帧规格（训练与推理必须一致） ---------- */
#define CAM_GRAY_W      96
#define CAM_GRAY_H      96
#define CAM_GRAY_SIZE   (CAM_GRAY_W * CAM_GRAY_H)   /* 9216 B */

/* ---------- 时域窗口：6 帧 @15fps ≈ 400 ms ---------- */
#define CAM_WIN_FRAMES  6
#define CAM_WIN_SIZE    (CAM_WIN_FRAMES * CAM_GRAY_SIZE)   /* 55296 B */
#define CAM_WIN_FPS     15

/* ---------- 引脚（见 docs/硬件接线原理图.md §4.1） ---------- */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0      11   /* 座子 D2 / Y2 */
#define CAM_PIN_D1      9    /* 座子 D3 / Y3 */
#define CAM_PIN_D2      8    /* 座子 D4 / Y4 */
#define CAM_PIN_D3      10   /* 座子 D5 / Y5 */
#define CAM_PIN_D4      12   /* 座子 D6 / Y6 */
#define CAM_PIN_D5      18   /* 座子 D7 / Y7 */
#define CAM_PIN_D6      17   /* 座子 D8 / Y8 */
#define CAM_PIN_D7      16   /* 座子 D9 / Y9 */
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

/** 相机初始化参数（不暴露 esp32-camera 的类型，保持调用方零依赖） */
typedef struct {
    int  width;      /* 期望宽度，默认 96 */
    int  height;     /* 期望高度，默认 96 */
    int  xclk_hz;    /* 0 → 20 MHz，失败自动回退 10 MHz */
    int  fb_count;   /* 帧缓冲数量，默认 3（时域需要连续取帧） */
    bool use_psram;  /* 帧缓冲放 PSRAM，默认 true */
} camera_driver_cfg_t;

#define CAMERA_DRIVER_CFG_DEFAULT  { CAM_GRAY_W, CAM_GRAY_H, 0, 3, true }

/**
 * @brief 初始化相机
 *
 * 目标配置：96x96 + 灰度 + 帧缓冲在 PSRAM。
 * 内部降级链：XCLK 20MHz → 10MHz → 分辨率降为 240x240（grab 时中心裁剪）。
 *
 * @param cfg NULL 时使用 CAMERA_DRIVER_CFG_DEFAULT
 */
esp_err_t camera_driver_init(const camera_driver_cfg_t *cfg);

esp_err_t camera_driver_deinit(void);

/** 打印传感器 PID / 实际分辨率 / 格式 / 灰度是否原生 */
esp_err_t camera_driver_info(void);

/** 实际生效的宽高（降级后可能与 96x96 不同） */
int camera_driver_width(void);
int camera_driver_height(void);

/** true 表示驱动直接给 1B/px 灰度；false 表示走了 YUV422 取 Y 兜底 */
bool camera_driver_gray_native(void);

/**
 * @brief 抓取一帧并转成 96x96 灰度写入 dst
 *
 * 内部处理：原生灰度直接拷贝；YUV422 则逐像素取 Y；
 * 分辨率大于 96x96 时中心裁剪（最近邻）。
 *
 * @param dst     目标缓冲，长度必须 >= CAM_GRAY_SIZE
 * @param dst_len 目标缓冲长度
 */
esp_err_t camera_driver_grab_gray(uint8_t *dst, size_t dst_len);

/** 清空时域窗口状态 */
esp_err_t camera_driver_window_reset(void);

/**
 * @brief 凑满 CAM_WIN_FRAMES 帧灰度并输出
 *
 * 按 CAM_WIN_FPS 节拍采帧（多余帧丢弃），输出按**时间顺序**排列：
 * out[0] 最早、out[WIN-1] 最新，与训练侧窗口顺序一致。
 *
 * @param out     输出缓冲，长度必须 >= CAM_WIN_SIZE
 * @param out_len 缓冲长度
 * @param fps_out 实际窗口帧率，可为 NULL
 */
esp_err_t camera_driver_window_grab(uint8_t *out, size_t out_len, float *fps_out);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRIVER_H */
