#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "capture_recorder.h"
#include "gseq_writer.h"
#include "sd_index.h"

static const char *TAG = "REC";

/* 环形缓冲（预卷） */
static uint8_t *s_ring;                 /* REC_RING_FRAMES × 9216 */
static uint32_t s_ring_ts[REC_RING_FRAMES];
static int      s_ring_head;            /* 下一个写入位置 */
static int      s_ring_filled;

/* 录制缓冲 */
static uint8_t *s_clip;                 /* REC_MAX_FRAMES × 9216 */
static uint16_t s_clip_ts[REC_MAX_FRAMES];
static uint16_t s_clip_n;

static rec_state_t s_state = REC_IDLE;
static rec_job_t   s_job;
static uint32_t    s_first_ts;
static int32_t     s_last_err;
static char        s_last_name[16];
static uint32_t    s_last_bytes;
static uint16_t    s_last_frames;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;        /* 落盘任务结束信号 */
static volatile bool     s_write_req;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------ */
/* 落盘任务：跑在 core1，避免与取帧争 core0 */
static void writer_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(200)) != pdTRUE) continue;

        rec_job_t job = s_job;
        uint16_t  n   = s_clip_n;
        uint32_t  ts  = s_first_ts;

        uint32_t id = sd_index_claim_id();
        char name[16];
        snprintf(name, sizeof(name), "data%04u", (unsigned)id);

        char dir[64], path[80];
        snprintf(dir, sizeof(dir), SD_INDEX_ROOT "/%s", name);
        snprintf(path, sizeof(path), "%s/%s", dir, SD_INDEX_CLIP_NAME);

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        if (mkdir(dir, 0775) != 0 && errno != EEXIST) err = ESP_FAIL;

        uint32_t bytes = 0;
        if (err == ESP_OK) {
            err = gseq_write(path, s_clip, n, s_clip_ts, &job, ts, &bytes);
        }
        if (err == ESP_OK) {
            err = gseq_verify(path, n);
        }

        if (err == ESP_OK) {
            sd_group_t g;
            memset(&g, 0, sizeof(g));
            snprintf(g.name, sizeof(g.name), "%s", name);
            g.id = id;
            strncpy(g.label, job.label, sizeof(g.label) - 1);
            memcpy(g.vars, job.vars, sizeof(g.vars));
            g.frames = n;
            g.bytes = bytes;
            g.mtime = now_ms() / 1000;
            g.flags = SD_GROUP_FLAG_OK;
            sd_index_append(&g);
        }

        int64_t dt = (esp_timer_get_time() - t0) / 1000;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (err == ESP_OK) {
            s_last_err = 0;
            strncpy(s_last_name, name, sizeof(s_last_name) - 1);
            s_last_bytes  = bytes;
            s_last_frames = n;
            s_state = REC_IDLE;
            ESP_LOGI(TAG, "落盘完成 %s: %u 帧 %u B, 耗时 %lld ms",
                     name, n, (unsigned)bytes, (long long)dt);
        } else {
            s_last_err = err;
            strncpy(s_last_name, name, sizeof(s_last_name) - 1);
            s_state = REC_ERROR;
            ESP_LOGE(TAG, "落盘失败 %s: %s", name, esp_err_to_name(err));
        }
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ */
esp_err_t rec_init(void)
{
    if (s_ring) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) return ESP_ERR_NO_MEM;

    s_ring = heap_caps_malloc((size_t)REC_RING_FRAMES * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_clip = heap_caps_malloc((size_t)REC_MAX_FRAMES * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_ring || !s_clip) {
        ESP_LOGE(TAG, "缓冲分配失败（需要 %u KB）",
                 (unsigned)(((size_t)REC_RING_FRAMES + REC_MAX_FRAMES) * REC_FRAME_BYTES / 1024));
        return ESP_ERR_NO_MEM;
    }
    s_ring_head = 0;
    s_ring_filled = 0;
    s_state = REC_IDLE;

    xTaskCreatePinnedToCore(writer_task, "rec_wr", 6144, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "初始化完成: 环形 %d 帧(%.1f KB) + 录制 %d 帧(%.1f KB)",
             REC_RING_FRAMES, (double)REC_RING_FRAMES * REC_FRAME_BYTES / 1024,
             REC_MAX_FRAMES, (double)REC_MAX_FRAMES * REC_FRAME_BYTES / 1024);
    return ESP_OK;
}

esp_err_t rec_pump_frame(const uint8_t *gray)
{
    if (!s_ring || !gray) return ESP_ERR_INVALID_ARG;
    uint32_t ts = now_ms();

    /* 1) 始终写环形缓冲 */
    memcpy(s_ring + (size_t)s_ring_head * REC_FRAME_BYTES, gray, REC_FRAME_BYTES);
    s_ring_ts[s_ring_head] = ts;
    s_ring_head = (s_ring_head + 1) % REC_RING_FRAMES;
    if (s_ring_filled < REC_RING_FRAMES) s_ring_filled++;

    /* 2) 录制中则追加 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == REC_RECORDING) {
        if (s_clip_n < REC_MAX_FRAMES) {
            memcpy(s_clip + (size_t)s_clip_n * REC_FRAME_BYTES, gray, REC_FRAME_BYTES);
            s_clip_ts[s_clip_n] = (uint16_t)(ts - s_first_ts);
            s_clip_n++;
            if (s_clip_n >= s_job.frames) {
                s_state = REC_WRITING;
                xSemaphoreGive(s_lock);
                xSemaphoreGive(s_done);
                return ESP_OK;
            }
        } else {
            s_state = REC_WRITING;
            xSemaphoreGive(s_lock);
            xSemaphoreGive(s_done);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t rec_start(const rec_job_t *job)
{
    if (!s_ring || !job) return ESP_ERR_INVALID_ARG;
    if (job->frames == 0 || job->frames > REC_MAX_FRAMES) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != REC_IDLE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_job = *job;
    s_clip_n = 0;
    s_last_err = 0;

    /* 预卷：把环形缓冲里最近 K 帧按时间顺序搬到录制缓冲 */
    /* 预卷只取片段的 1/4（最多 15 帧），其余留给"按下之后"的真实动作 */
    int max_pre = REC_RING_FRAMES;
    int quarter = job->frames / 4;
    if (quarter < max_pre) max_pre = quarter;
    int pre = (max_pre < s_ring_filled) ? max_pre : s_ring_filled;
    if (pre > job->frames) pre = job->frames;
    if (pre > 0) {
        int start = (s_ring_head - pre + REC_RING_FRAMES) % REC_RING_FRAMES;
        s_first_ts = s_ring_ts[start];
        for (int i = 0; i < pre; i++) {
            int idx = (start + i) % REC_RING_FRAMES;
            memcpy(s_clip + (size_t)s_clip_n * REC_FRAME_BYTES,
                   s_ring + (size_t)idx * REC_FRAME_BYTES, REC_FRAME_BYTES);
            /* 存相对首帧的毫秒偏移，供 .gseq v2 的 delta_ms[] 直接使用 */
            s_clip_ts[s_clip_n] = (uint16_t)(s_ring_ts[idx] - s_first_ts);
            s_clip_n++;
        }
    } else {
        s_first_ts = now_ms();
    }

    /* 预卷已填满整段（极小片段）时直接进入落盘，避免多录 1 帧 */
    bool full = (s_clip_n >= s_job.frames);
    s_state = full ? REC_WRITING : REC_RECORDING;
    xSemaphoreGive(s_lock);

    if (full) xSemaphoreGive(s_done);

    ESP_LOGI(TAG, "开始录制: %s %u 帧（预卷 %d%s）", job->label, job->frames, pre,
             full ? "，已满" : "");
    return ESP_OK;
}

rec_state_t rec_state(rec_progress_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rec_state_t st = s_state;
    if (out) {
        out->state = st;
        out->got = s_clip_n;
        out->total = s_job.frames;
        out->last_err = s_last_err;
        memcpy(out->last_name, s_last_name, sizeof(out->last_name));
        out->last_bytes = s_last_bytes;
        out->last_frames = s_last_frames;
    }
    xSemaphoreGive(s_lock);
    return st;
}

rec_state_t rec_wait_idle(int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (1) {
        rec_progress_t p;
        rec_state_t st = rec_state(&p);
        if (st == REC_IDLE || st == REC_ERROR) return st;
        if (timeout_ms > 0 && (esp_timer_get_time() - t0) / 1000 > timeout_ms) return st;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

const uint8_t *rec_last_clip(uint16_t *frames, rec_job_t *meta)
{
    if (!s_clip) return NULL;
    if (frames) *frames = s_last_frames;
    if (meta)   *meta = s_job;
    return s_clip;
}

void rec_discard_last(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_frames = 0;
    s_last_bytes = 0;
    s_last_name[0] = '\0';
    xSemaphoreGive(s_lock);
}
