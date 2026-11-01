/* web_ui 实现
 *
 * 预览方式说明（与文档的差异，已在 docs/网页采集终端.md 记录原因）：
 *   原计划用 WebSocket 推流，但 esp_http_server 是**单任务**模型——WS 长连接的
 *   处理函数必须一直阻塞在 recv 上才能保活，会把 /api/status 等接口一起卡死。
 *   改为 **HTTP 轮询**：页面每帧 GET /api/frame 取 9216 B 原始灰度。
 *   12 fps × 9 KB ≈ 110 KB/s，开销可接受，且各接口互不影响。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "web_ui.h"
#include "capture_recorder.h"
#include "sd_index.h"
#include "sd_files_driver.h"
#include "wifi_sta.h"
#include "cfg_store.h"

static const char *TAG = "WEB";

/* 删除任务状态（h_status 需要，故提前声明） */
typedef struct {
    volatile bool busy;
    volatile int  done;
    volatile int  total;
} del_state_t;
static del_state_t s_del;

#define FRAME_BYTES   9216
#define HDR_BYTES     8

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t   s_hd;
static uint8_t         *s_frame;          /* 最新一帧（PSRAM） */
static SemaphoreHandle_t s_lock;
static volatile bool    s_preview_on = true;
static volatile int     s_preview_fps = 12;
static volatile uint32_t s_sent;
static volatile bool  s_preview_auto_off;   /* 因录制而关闭，结束后自动恢复 */

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------ */
static esp_err_t h_index(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t h_config(httpd_req_t *req)
{
    const cfg_t *c = cfg_get();
    cJSON *root = cJSON_CreateObject();

    char hex[12];
    cfg_crc_hex(hex, sizeof(hex));
    cJSON_AddStringToObject(root, "cfg_crc", hex);

    cJSON *types = cJSON_CreateArray();
    for (int i = 0; i < c->type_count; i++) {
        cJSON_AddItemToArray(types, cJSON_CreateString(c->types[i]));
    }
    cJSON_AddItemToObject(root, "types", types);

    cJSON *vars = cJSON_CreateArray();
    for (int v = 0; v < c->var_count; v++) {
        const cfg_var_t *cv = &c->vars[v];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", cv->name);
        cJSON_AddNumberToObject(o, "max", cv->max);
        cJSON *labs = cJSON_CreateArray();
        for (int i = 1; i <= cv->max; i++) {
            if (!cv->has_label[i]) continue;
            cJSON *l = cJSON_CreateObject();
            cJSON_AddNumberToObject(l, "code", i);
            cJSON_AddStringToObject(l, "name", cv->label[i]);
            cJSON_AddItemToArray(labs, l);
        }
        cJSON_AddItemToObject(o, "labels", labs);
        cJSON_AddItemToArray(vars, o);
    }
    cJSON_AddItemToObject(root, "variables", vars);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *wifi = cJSON_CreateObject();
    char ip[20] = "-";
    wifi_sta_get_ip(ip, sizeof(ip));
    cJSON_AddBoolToObject(wifi, "connected", wifi_sta_is_connected());
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddNumberToObject(wifi, "rssi", wifi_sta_get_rssi());
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON *cam = cJSON_CreateObject();
    cJSON_AddNumberToObject(cam, "w", 96);
    cJSON_AddNumberToObject(cam, "h", 96);
    cJSON_AddItemToObject(root, "cam", cam);

    cJSON *sd = cJSON_CreateObject();
    uint64_t total = 0, freeb = 0;
    bool mounted = sd_files_driver_is_mounted() &&
                   (sd_files_driver_get_space(&total, &freeb) == ESP_OK);
    cJSON_AddBoolToObject(sd, "mounted", mounted);
    cJSON_AddNumberToObject(sd, "free_kb", (double)(freeb / 1024));
    cJSON_AddItemToObject(root, "sd", sd);

    cJSON *rec = cJSON_CreateObject();
    rec_progress_t p;
    rec_state_t st = rec_state(&p);
    const char *stn = (st == REC_IDLE) ? "idle" :
                      (st == REC_RECORDING) ? "recording" :
                      (st == REC_WRITING) ? "writing" : "error";
    cJSON_AddStringToObject(rec, "state", stn);
    cJSON_AddNumberToObject(rec, "got", p.got);
    cJSON_AddNumberToObject(rec, "total", p.total);
    cJSON_AddStringToObject(rec, "last", p.last_name);
    cJSON_AddNumberToObject(rec, "last_frames", p.last_frames);
    cJSON_AddNumberToObject(rec, "last_bytes", p.last_bytes);
    cJSON_AddItemToObject(root, "rec", rec);

    /* 录制结束自动恢复预览 */
    if (s_preview_auto_off && (st == REC_IDLE || st == REC_ERROR)) {
        s_preview_on = true;
        s_preview_auto_off = false;
    }

    cJSON *pv = cJSON_CreateObject();
    cJSON_AddBoolToObject(pv, "on", s_preview_on);
    cJSON_AddNumberToObject(pv, "fps", s_preview_fps);
    cJSON_AddNumberToObject(pv, "sent", s_sent);
    cJSON_AddItemToObject(root, "preview", pv);

    char crchex[12];
    cfg_crc_hex(crchex, sizeof(crchex));
    cJSON_AddStringToObject(root, "cfg_crc", crchex);
    cJSON *del = cJSON_CreateObject();
    cJSON_AddBoolToObject(del, "busy", s_del.busy);
    cJSON_AddNumberToObject(del, "done", s_del.done);
    cJSON_AddNumberToObject(del, "total", s_del.total);
    cJSON_AddItemToObject(root, "del", del);
    cJSON_AddNumberToObject(root, "groups", sd_index_count());
    cJSON_AddNumberToObject(root, "next_id", sd_index_next_id());
    cJSON_AddNumberToObject(root, "heap_internal",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "uptime_ms", now_ms());

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    cJSON_Delete(root);
    return err;
}

/* 预览帧：8 字节头 + 9216 字节灰度 */
static esp_err_t h_frame(httpd_req_t *req)
{
    if (!s_preview_on) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    static uint8_t *tx;                 /* 内部 RAM，避免 PSRAM 参与 TCP 发送路径 */
    if (!tx) {
        tx = heap_caps_malloc(HDR_BYTES + FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!tx) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    uint32_t seq = s_sent;
    uint16_t fps = (uint16_t)s_preview_fps;
    tx[0] = seq & 0xFF; tx[1] = (seq >> 8) & 0xFF;
    tx[2] = (seq >> 16) & 0xFF; tx[3] = (seq >> 24) & 0xFF;
    tx[4] = fps & 0xFF; tx[5] = (fps >> 8) & 0xFF;
    tx[6] = 0; tx[7] = 0;

    if (s_frame) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(tx + HDR_BYTES, s_frame, FRAME_BYTES);
        xSemaphoreGive(s_lock);
    } else {
        memset(tx + HDR_BYTES, 0, FRAME_BYTES);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    s_sent++;
    return httpd_resp_send(req, (const char *)tx, HDR_BYTES + FRAME_BYTES);
}

/* 读 JSON body */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int len = req->content_len;
    if (len <= 0 || (size_t)len >= cap) return -1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[len] = '\0';
    return len;
}

static esp_err_t h_record(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"body\"}");
    }

    rec_progress_t p;
    if (rec_state(&p) != REC_IDLE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"err\":\"busy\"}");
    }

    const cfg_t *cf = cfg_get();
    rec_job_t job;
    memset(&job, 0, sizeof(job));
    job.frames = REC_DYNAMIC_FRAMES;
    job.var_count = cf->var_count;
    job.cfg_crc = cf->crc32;
    strcpy(job.label, "none");

    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *it;
        if ((it = cJSON_GetObjectItem(root, "label")) && cJSON_IsString(it))
            strncpy(job.label, it->valuestring, sizeof(job.label) - 1);
        if ((it = cJSON_GetObjectItem(root, "frames")) && cJSON_IsNumber(it))
            job.frames = (uint16_t)it->valuedouble;
        cJSON *vars = cJSON_GetObjectItem(root, "vars");
        if (cJSON_IsArray(vars)) {
            int i = 0;
            cJSON *v = NULL;
            cJSON_ArrayForEach(v, vars) {
                if (i >= 4) break;
                if (cJSON_IsNumber(v)) job.vars[i++] = (uint8_t)v->valuedouble;
            }
        }
        cJSON_Delete(root);
    }

    /* 类别必须存在；变量不得越界 */
    if (cfg_type_index(job.label) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"unknown label\"}");
    }
    if (cfg_validate_vars(job.vars, job.var_count) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"vars out of range\"}");
    }

    if (job.frames == 0 || job.frames > REC_MAX_FRAMES) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"frames\"}");
    }

    /* 采样期间关闭图传（D4） */
    s_preview_on = false;
    s_preview_auto_off = true;

    esp_err_t err = rec_start(&job);
    if (err != ESP_OK) {
        s_preview_on = true;
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"err\":\"rec_start\"}");
    }

    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_preview(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *it;
        if ((it = cJSON_GetObjectItem(root, "on")) && cJSON_IsBool(it))
            s_preview_on = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(root, "fps")) && cJSON_IsNumber(it)) {
            int f = (int)it->valuedouble;
            s_preview_fps = (f < 1) ? 1 : (f > 25 ? 25 : f);
        }
        cJSON_Delete(root);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* 回放：最近一段的帧数据（头部 8 字节：帧数 u16 + 保留） */
static esp_err_t h_clip_last(httpd_req_t *req)
{
    uint16_t n = 0;
    rec_job_t meta;
    const uint8_t *clip = rec_last_clip(&n, &meta);
    if (!clip || n == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"err\":\"no clip\"}");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char hdr[8];
    hdr[0] = n & 0xFF; hdr[1] = (n >> 8) & 0xFF;
    hdr[2] = 25; hdr[3] = 0;
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    esp_err_t err = httpd_resp_send_chunk(req, hdr, sizeof(hdr));
    if (err != ESP_OK) return err;

    size_t total = (size_t)n * FRAME_BYTES;
    size_t sent = 0;
    while (sent < total) {
        size_t chunk = total - sent;
        if (chunk > 16384) chunk = 16384;
        err = httpd_resp_send_chunk(req, (const char *)(clip + sent), chunk);
        if (err != ESP_OK) return err;
        sent += chunk;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* ================= 数据管理（S4.5） ================= */
#define DEL_MAX 32

static char s_del_names[DEL_MAX][16];
static int  s_del_n;
static char s_q_label[16];

static bool valid_group_name(const char *n)
{
    if (strncmp(n, "data", 4) != 0) return false;
    const char *p = n + 4;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '\0';
}

/* GET /api/groups?label=&offset=&limit= */
static esp_err_t h_groups(httpd_req_t *req)
{
    char q[96], val[24];
    int offset = 0, limit = 50;
    const char *label = NULL;
    s_q_label[0] = '\0';

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "offset", val, sizeof(val)) == ESP_OK) {
            offset = atoi(val);
            if (offset < 0) offset = 0;
        }
        if (httpd_query_key_value(q, "limit", val, sizeof(val)) == ESP_OK) {
            limit = atoi(val);
            if (limit < 1) limit = 1;
            if (limit > 200) limit = 200;
        }
        if (httpd_query_key_value(q, "label", val, sizeof(val)) == ESP_OK &&
            val[0] && strcmp(val, "all") != 0) {
            strncpy(s_q_label, val, sizeof(s_q_label) - 1);
            s_q_label[sizeof(s_q_label) - 1] = '\0';
            label = s_q_label;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int total = 0, emitted = 0;

    for (int i = 0; i < sd_index_count(); i++) {
        const sd_group_t *g = sd_index_get(i);
        if (!g) continue;
        if (label && strcmp(g->label, label) != 0) continue;
        if (total >= offset && emitted < limit) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", g->name);
            cJSON_AddStringToObject(o, "label", g->label);
            cJSON *v = cJSON_CreateArray();
            for (int k = 0; k < 4; k++) cJSON_AddItemToArray(v, cJSON_CreateNumber(g->vars[k]));
            cJSON_AddItemToObject(o, "vars", v);
            cJSON_AddNumberToObject(o, "frames", g->frames);
            cJSON_AddNumberToObject(o, "bytes", g->bytes);
            cJSON_AddNumberToObject(o, "mtime", g->mtime);
            cJSON_AddNumberToObject(o, "flags", g->flags);
            cJSON_AddItemToArray(arr, o);
            emitted++;
        }
        total++;
    }

    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "limit", limit);
    cJSON_AddItemToObject(root, "groups", arr);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    cJSON_Delete(root);
    return err;
}

/* GET /api/group?name=data0007 —— 与 /api/clip/last 同格式：8B 头 + N×9216 */
static esp_err_t h_group(httpd_req_t *req)
{
    char q[64], name[16] = "", rawv[8] = "";
    bool want_raw = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
        !valid_group_name(name)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"name\"}");
    }
    if (httpd_query_key_value(q, "raw", rawv, sizeof(rawv)) == ESP_OK &&
        rawv[0] == '1') {
        want_raw = true;
    }

    const sd_group_t *g = sd_index_find(name);
    if (!g) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"err\":\"not found\"}");
    }

    char path[80];
    snprintf(path, sizeof(path), SD_INDEX_ROOT "/%.15s/%s", name, SD_INDEX_CLIP_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"err\":\"no file\"}");
    }

    if (want_raw) {
        /* raw=1：原样返回 .gseq 文件（PC 侧工具需要 delta_ms 等完整头信息） */
        static uint8_t *rbuf;
        if (!rbuf) rbuf = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!rbuf) { fclose(f); return ESP_FAIL; }
        httpd_resp_set_type(req, "application/octet-stream");
        size_t got;
        while ((got = fread(rbuf, 1, 16384, f)) > 0) {
            if (httpd_resp_send_chunk(req, (const char *)rbuf, got) != ESP_OK) break;
        }
        fclose(f);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    /* 解析 v3 头，算出帧数据偏移 */
    uint8_t hdr[26 + 32];
    if (fread(hdr, 1, 26, f) != 26 || memcmp(hdr, "GSEQ", 4) != 0) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"err\":\"bad clip\"}");
    }
    uint8_t label_len = hdr[5];
    uint16_t n = (uint16_t)(hdr[10] | (hdr[11] << 8));
    long data_off = 26 + label_len + 2L * n;
    if (fseek(f, data_off, SEEK_SET) != 0) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"err\":\"seek\"}");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    uint8_t oh[8] = { (uint8_t)(n & 0xFF), (uint8_t)(n >> 8), 25, 0, 0, 0, 0, 0 };
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)oh, 8);
    if (err != ESP_OK) { fclose(f); return err; }

    static uint8_t *buf;
    if (!buf) buf = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return ESP_FAIL; }

    size_t total = (size_t)n * FRAME_BYTES;
    while (total > 0) {
        size_t want = total < 16384 ? total : 16384;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        err = httpd_resp_send_chunk(req, (const char *)buf, got);
        if (err != ESP_OK) break;
        total -= got;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* 删除任务：逐组 unlink+rmdir，最后重写索引（core1） */
static void del_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < s_del_n; i++) {
        char dir[64], file[80];
        snprintf(dir, sizeof(dir), SD_INDEX_ROOT "/%.15s", s_del_names[i]);
        snprintf(file, sizeof(file), "%.63s/%s", dir, SD_INDEX_CLIP_NAME);
        unlink(file);
        rmdir(dir);
        sd_index_remove(s_del_names[i]);
        s_del.done = i + 1;
        ESP_LOGW(TAG, "已删除 %s（%d/%d）", s_del_names[i], s_del.done, s_del.total);
    }
    sd_index_rewrite();
    ESP_LOGW(TAG, "删除完成，共 %d 组；索引已重写", s_del.total);
    s_del.busy = false;
    vTaskDelete(NULL);
}

/* POST /api/group/delete  {names:["data0007",...]} */
static esp_err_t h_group_delete(httpd_req_t *req)
{
    rec_progress_t rp;
    if (rec_state(&rp) != REC_IDLE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"err\":\"recording\"}");
    }
    if (s_del.busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"err\":\"busy\"}");
    }

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    cJSON *arr = root ? cJSON_GetObjectItem(root, "names") : NULL;
    int n = 0;
    if (cJSON_IsArray(arr)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsString(it)) continue;
            if (n >= DEL_MAX) break;
            const char *s = it->valuestring;
            if (!valid_group_name(s)) continue;
            if (!sd_index_find(s)) {
                ESP_LOGW(TAG, "跳过不存在的组 %s", s);
                continue;
            }
            strncpy(s_del_names[n], s, sizeof(s_del_names[n]) - 1);
            s_del_names[n][sizeof(s_del_names[n]) - 1] = '\0';
            n++;
        }
    }
    if (root) cJSON_Delete(root);

    if (n == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"err\":\"no valid names\"}");
    }

    s_del_n = n;
    s_del.total = n;
    s_del.done = 0;
    s_del.busy = true;
    xTaskCreatePinnedToCore(del_task, "del", 4096, NULL, 3, NULL, 1);

    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/group/rescan */
static esp_err_t h_group_rescan(httpd_req_t *req)
{
    int rc = sd_index_rescan();
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":%s,\"groups\":%d}", rc == 0 ? "true" : "false", sd_index_count());
    return httpd_resp_sendstr(req, out);
}

/* GET /api/stats —— 每类组数与小计 */
static esp_err_t h_stats(httpd_req_t *req)
{
    const cfg_t *c = cfg_get();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < c->type_count; i++) {
        int cnt = 0;
        unsigned long bytes = 0;
        for (int k = 0; k < sd_index_count(); k++) {
            const sd_group_t *g = sd_index_get(k);
            if (g && strcmp(g->label, c->types[i]) == 0) { cnt++; bytes += g->bytes; }
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", c->types[i]);
        cJSON_AddNumberToObject(o, "count", cnt);
        cJSON_AddNumberToObject(o, "bytes", (double)bytes);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "stats", arr);
    cJSON *del = cJSON_CreateObject();
    cJSON_AddBoolToObject(del, "busy", s_del.busy);
    cJSON_AddNumberToObject(del, "done", s_del.done);
    cJSON_AddNumberToObject(del, "total", s_del.total);
    cJSON_AddItemToObject(root, "del", del);
    cJSON_AddNumberToObject(root, "groups", sd_index_count());
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    cJSON_Delete(root);
    return err;
}

esp_err_t web_ui_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    if (!s_frame) {
        s_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_frame) s_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_INTERNAL);
        if (!s_frame) return ESP_ERR_NO_MEM;
        memset(s_frame, 0, FRAME_BYTES);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 16;   /* 默认只有 8，路由已到 12 个 */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_hd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = h_config },
        { .uri = "/api/frame",    .method = HTTP_GET,  .handler = h_frame },
        { .uri = "/api/record",   .method = HTTP_POST, .handler = h_record },
        { .uri = "/api/preview",  .method = HTTP_POST, .handler = h_preview },
        { .uri = "/api/clip/last",.method = HTTP_GET,  .handler = h_clip_last },
        { .uri = "/api/groups",   .method = HTTP_GET,  .handler = h_groups },
        { .uri = "/api/group",    .method = HTTP_GET,  .handler = h_group },
        { .uri = "/api/group/delete", .method = HTTP_POST, .handler = h_group_delete },
        { .uri = "/api/group/rescan", .method = HTTP_POST, .handler = h_group_rescan },
        { .uri = "/api/stats",    .method = HTTP_GET,  .handler = h_stats },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_hd, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP 服务已启动，端口 80（预览：HTTP 轮询，%d fps 上限）", s_preview_fps);
    return ESP_OK;
}

void web_ui_push_frame(const uint8_t *gray)
{
    if (!gray || !s_frame || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_frame, gray, FRAME_BYTES);
    xSemaphoreGive(s_lock);
}

bool web_ui_preview_enabled(void) { return s_preview_on; }

void web_ui_set_preview(bool on) { s_preview_on = on; }

void web_ui_set_preview_fps(int fps)
{
    if (fps < 0) fps = 0;
    if (fps > 25) fps = 25;
    s_preview_fps = fps;
}

int web_ui_ws_clients(void) { return 0; }   /* 轮询模式无长连接 */

uint32_t web_ui_preview_sent(void) { return s_sent; }
