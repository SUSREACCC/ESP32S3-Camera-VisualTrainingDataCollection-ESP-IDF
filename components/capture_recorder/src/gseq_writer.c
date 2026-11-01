#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "gseq_writer.h"

static const char *TAG = "GSEQ";

#define GSEQ_VERSION 3
#define GSEQ_HDR_FIXED 26      /* v3 固定头长度 */
#define BOUNCE_BYTES (32 * 1024)      /* 内部 DMA 中转块 */

static uint8_t *s_bounce;             /* 内部 RAM，DMA 能力 */

static esp_err_t ensure_bounce(void)
{
    if (s_bounce) return ESP_OK;
    s_bounce = heap_caps_malloc(BOUNCE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_bounce) {
        ESP_LOGE(TAG, "DMA 中转缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t gseq_write(const char *path,
                     const uint8_t *frames,
                     uint16_t n,
                     const uint16_t *delta_ms,
                     const rec_job_t *meta,
                     uint32_t first_ts_ms,
                     uint32_t *out_bytes)
{
    if (!path || !frames || !n || !meta || !delta_ms) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_bounce();
    if (err != ESP_OK) return err;

    uint8_t label_len = (uint8_t)strnlen(meta->label, sizeof(meta->label));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return ESP_FAIL;
    }

    /* ---- 头（v3：26 B 固定 + label）---- */
    uint8_t hdr[GSEQ_HDR_FIXED + 32];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "GSEQ", 4);
    hdr[4]  = GSEQ_VERSION;
    hdr[5]  = label_len;
    hdr[6]  = 96;
    hdr[7]  = 96;
    hdr[8]  = 25;
    hdr[9]  = 0;
    put_u16(&hdr[10], n);
    put_u32(&hdr[12], first_ts_ms);
    put_u32(&hdr[16], meta->cfg_crc);       /* v3 新增 */
    hdr[20] = meta->var_count;              /* v3 新增 */
    hdr[21] = 0;                            /* flags */
    for (int i = 0; i < 4; i++) hdr[22 + i] = meta->vars[i];  /* v3 新增 */
    memcpy(&hdr[GSEQ_HDR_FIXED], meta->label, label_len);

    size_t hdr_len = GSEQ_HDR_FIXED + label_len;
    if (fwrite(hdr, 1, hdr_len, f) != hdr_len) { fclose(f); return ESP_FAIL; }

    /* ---- delta_ms ---- */
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b[2];
        put_u16(b, delta_ms[i]);
        if (fwrite(b, 1, 2, f) != 2) { fclose(f); return ESP_FAIL; }
    }

    /* ---- 帧数据：PSRAM → 内部中转 → 文件 ---- */
    size_t total = (size_t)n * REC_FRAME_BYTES;
    size_t done = 0;
    while (done < total) {
        size_t chunk = total - done;
        if (chunk > BOUNCE_BYTES) chunk = BOUNCE_BYTES;
        memcpy(s_bounce, frames + done, chunk);
        if (fwrite(s_bounce, 1, chunk, f) != chunk) {
            ESP_LOGE(TAG, "写帧数据失败 @%u/%u", (unsigned)done, (unsigned)total);
            fclose(f);
            return ESP_FAIL;
        }
        done += chunk;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (out_bytes) *out_bytes = (uint32_t)(hdr_len + 2u * n + total);
    return ESP_OK;
}

esp_err_t gseq_verify(const char *path, uint16_t expect_frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;

    uint8_t hdr[GSEQ_HDR_FIXED + 32];
    if (fread(hdr, 1, GSEQ_HDR_FIXED, f) != GSEQ_HDR_FIXED) { fclose(f); return ESP_FAIL; }

    if (memcmp(hdr, "GSEQ", 4) != 0) { fclose(f); return ESP_ERR_INVALID_RESPONSE; }
    if (hdr[4] != GSEQ_VERSION) { fclose(f); return ESP_ERR_INVALID_VERSION; }

    uint8_t label_len = hdr[5];
    uint16_t n = get_u16(&hdr[10]);
    if (n != expect_frames) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    /* 校验文件长度 */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long expect = (long)(GSEQ_HDR_FIXED + label_len + 2u * n + (size_t)n * REC_FRAME_BYTES);
    fclose(f);
    if (size != expect) {
        ESP_LOGE(TAG, "%s 长度 %ld != 期望 %ld", path, size, expect);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
