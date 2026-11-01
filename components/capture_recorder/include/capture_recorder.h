/* capture_recorder —— 取帧泵 + 环形缓冲 + 录制状态机
 *
 * 设计依据（docs/相机帧率实测.md、docs/SD目录结构与数据管理.md）：
 *   - 相机原生 25 fps / 40 ms，**不抽取**，帧间隔天然均匀
 *   - 环形缓冲保留 0.6 s（15 帧）预卷，覆盖按钮延迟
 *   - 录制先存 PSRAM，录完再落盘（落盘在 core1，避开取帧核）
 *   - 落盘经内部 DMA 中转（S3 的 SDMMC 不能 DMA 读 PSRAM）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define REC_RING_FRAMES      15      /* 预卷 0.6 s @25fps */
#define REC_MAX_FRAMES       160     /* 单段最多 160 帧 = 6.4 s */
#define REC_FRAME_BYTES      9216    /* 96x96 灰度 */

#define REC_STATIC_FRAMES    12
#define REC_DYNAMIC_FRAMES   75

typedef enum {
    REC_IDLE = 0,
    REC_RECORDING,
    REC_WRITING,
    REC_ERROR,
} rec_state_t;

typedef struct {
    char     label[16];
    uint8_t  vars[4];       /* 4 个变量值（数字编码，0 = 未设置） */
    uint8_t  var_count;
    uint32_t cfg_crc;       /* 采集时生效的 config.json 指纹 */
    uint16_t frames;        /* 目标帧数，0 → REC_DYNAMIC_FRAMES */
} rec_job_t;

typedef struct {
    rec_state_t state;
    uint16_t    got;
    uint16_t    total;
    int32_t     last_err;
    char        last_name[16];
    uint32_t    last_bytes;
    uint16_t    last_frames;
} rec_progress_t;

/** 分配缓冲（PSRAM），启动落盘任务（core1） */
esp_err_t rec_init(void);

/** 泵任务每帧调用一次（core0）。内部完成：写环形缓冲 → 录制中则追加 */
esp_err_t rec_pump_frame(const uint8_t *gray);

/** 开始录制（含预卷）。非 IDLE 返回 ESP_ERR_INVALID_STATE */
esp_err_t rec_start(const rec_job_t *job);

/** 查询状态 */
rec_state_t rec_state(rec_progress_t *out);

/** 等待本次录制 + 落盘结束（IDLE 或 ERROR），返回最终状态 */
rec_state_t rec_wait_idle(int timeout_ms);

/** 最近一段的帧数据（用于回放），同时输出帧数与元数据 */
const uint8_t *rec_last_clip(uint16_t *frames, rec_job_t *meta);

/** 丢弃最近一段的回放缓存 */
void rec_discard_last(void);
