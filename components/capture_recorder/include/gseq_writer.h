/* .gseq v2 写入器
 *
 * 布局：
 *   0   4  magic "GSEQ"
 *   4   1  version = 2
 *   5   1  label_len
 *   6   1  width   7 height   8 fps_nominal   9 reserved
 *   10  2  frame_count (u16 LE)
 *   12  4  timestamp_ms (u32 LE，首帧)
 *   16  1  subject  17 light  18 bg  19 dist_cm
 *   20  1  angle_deg+128   21 flags
 *   22  L  label
 *   22+L 2N delta_ms[] (u16 LE，相对首帧)
 *   ...    frames: N × 9216
 *
 * 关键：记录缓冲在 PSRAM，而 S3 的 SDMMC 不能对 PSRAM 做 DMA，
 *       因此落盘必须经内部 DMA 中转缓冲，不能直接 fwrite(PSRAM)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "capture_recorder.h"

/** 写一个 .gseq 文件；返回写入字节数 */
esp_err_t gseq_write(const char *path,
                     const uint8_t *frames,     /* PSRAM，N × REC_FRAME_BYTES */
                     uint16_t n,
                     const uint16_t *delta_ms,  /* N 个 */
                     const rec_job_t *meta,
                     uint32_t first_ts_ms,
                     uint32_t *out_bytes);

/** 回读校验：magic/version/frame_count/文件长度 */
esp_err_t gseq_verify(const char *path, uint16_t expect_frames);
