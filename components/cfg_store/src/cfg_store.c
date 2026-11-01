#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "cJSON.h"

#include "cfg_store.h"

static const char *TAG = "CFG";

#define CFG_FILE_MAX  4096

static cfg_t   s_cfg;
static uint8_t s_buf[CFG_FILE_MAX];

/* ------------------------------------------------------------------ */
/* zlib 兼容的 CRC32：多项式 0xEDB88320，初值/终值异或 0xFFFFFFFF
   —— 必须与 PC 侧 Python 的 zlib.crc32 结果一致 */
static uint32_t crc32_zlib(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void copy_name(char *dst, const char *src)
{
    strncpy(dst, src, CFG_NAME_LEN - 1);
    dst[CFG_NAME_LEN - 1] = '\0';
}

/* ------------------------------------------------------------------ */
static void load_defaults(cfg_t *c)
{
    static const char *k_types[] = {
        "thumbs_up", "fist", "victory", "palm", "ok",
        "wave", "swipe_left", "swipe_right", "circle", "none",
    };
    static const struct { const char *name; uint8_t max;
                          const char *lab[6]; } k_vars[] = {
        { "light", 3, { NULL, "warm", "cool", "backlit", NULL, NULL } },
        { "bg",    2, { NULL, "plain", "cluttered", NULL, NULL, NULL } },
        { "dist",  3, { NULL, "20cm", "35cm", "50cm", NULL, NULL } },
        { "angle", 5, { NULL, "front", "plus15", "minus15", "plus30", "minus30" } },
    };

    memset(c, 0, sizeof(*c));
    c->type_count = (uint8_t)(sizeof(k_types) / sizeof(k_types[0]));
    for (int i = 0; i < c->type_count; i++) copy_name(c->types[i], k_types[i]);

    c->var_count = (uint8_t)(sizeof(k_vars) / sizeof(k_vars[0]));
    for (int v = 0; v < c->var_count; v++) {
        cfg_var_t *cv = &c->vars[v];
        copy_name(cv->name, k_vars[v].name);
        cv->max = k_vars[v].max;
        for (int i = 1; i <= cv->max; i++) {
            if (k_vars[v].lab[i]) {
                cv->has_label[i] = true;
                copy_name(cv->label[i], k_vars[v].lab[i]);
            }
        }
    }
    c->crc32 = 0;
    c->from_file = false;
}

/* ------------------------------------------------------------------ */
static void parse(const uint8_t *buf, size_t len, cfg_t *c)
{
    memset(c, 0, sizeof(*c));

    cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
    if (!root) {
        ESP_LOGE(TAG, "JSON 解析失败，回落内置默认");
        load_defaults(c);
        return;
    }

    /* types */
    cJSON *types = cJSON_GetObjectItem(root, "types");
    if (cJSON_IsArray(types)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, types) {
            if (!cJSON_IsString(it)) continue;
            if (c->type_count >= CFG_MAX_TYPES) {
                ESP_LOGW(TAG, "types 超过 %d 个，截断", CFG_MAX_TYPES);
                break;
            }
            const char *s = it->valuestring;
            if (!s || !s[0]) continue;
            if (strlen(s) >= CFG_NAME_LEN) ESP_LOGW(TAG, "类别名过长被截断: %s", s);
            /* 去重 */
            bool dup = false;
            for (int i = 0; i < c->type_count; i++) {
                if (strncmp(c->types[i], s, CFG_NAME_LEN) == 0) { dup = true; break; }
            }
            if (dup) { ESP_LOGW(TAG, "重复类别名，忽略: %s", s); continue; }
            copy_name(c->types[c->type_count++], s);
        }
    }
    if (c->type_count == 0) {
        ESP_LOGE(TAG, "types 为空，回落内置默认");
        cJSON_Delete(root);
        load_defaults(c);
        return;
    }

    /* variables */
    cJSON *vars = cJSON_GetObjectItem(root, "variables");
    if (cJSON_IsArray(vars)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, vars) {
            if (c->var_count >= CFG_MAX_VARS) {
                ESP_LOGW(TAG, "variables 超过 %d 个，截断", CFG_MAX_VARS);
                break;
            }
            cJSON *nm = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(nm) || !nm->valuestring[0]) continue;

            cfg_var_t *cv = &c->vars[c->var_count];
            copy_name(cv->name, nm->valuestring);

            uint8_t mx = 9;                     /* 缺省 max */
            cJSON *jm = cJSON_GetObjectItem(it, "max");
            if (cJSON_IsNumber(jm)) {
                int m = (int)jm->valuedouble;
                if (m < 1 || m > CFG_MAX_LABEL) {
                    ESP_LOGW(TAG, "%s.max=%d 越界，夹到 1..%d", cv->name, m, CFG_MAX_LABEL);
                    m = (m < 1) ? 1 : CFG_MAX_LABEL;
                }
                mx = (uint8_t)m;
            }
            cv->max = mx;

            cJSON *labs = cJSON_GetObjectItem(it, "labels");
            if (cJSON_IsObject(labs)) {
                cJSON *lp = NULL;
                cJSON_ArrayForEach(lp, labs) {
                    if (!cJSON_IsString(lp)) continue;
                    int code = atoi(lp->string);
                    if (code < 1 || code > mx) {
                        ESP_LOGW(TAG, "%s.labels 键 \"%s\" 超出 1..%d，忽略",
                                 cv->name, lp->string, mx);
                        continue;
                    }
                    if (strlen(lp->valuestring) >= CFG_NAME_LEN) {
                        ESP_LOGW(TAG, "%s.labels[\"%d\"] 过长，截断", cv->name, code);
                    }
                    cv->has_label[code] = true;
                    copy_name(cv->label[code], lp->valuestring);
                }
            }
            c->var_count++;
        }
    }

    c->crc32 = crc32_zlib(buf, len);
    c->from_file = true;
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
esp_err_t cfg_load(void)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "未找到 %s，使用内置默认", CFG_PATH);
        load_defaults(&s_cfg);
        return ESP_OK;
    }
    size_t n = fread(s_buf, 1, sizeof(s_buf), f);
    bool too_big = (n == sizeof(s_buf));
    fclose(f);

    if (n == 0) {
        ESP_LOGE(TAG, "config.json 为空，使用内置默认");
        load_defaults(&s_cfg);
        return ESP_OK;
    }
    if (too_big) ESP_LOGW(TAG, "config.json 超过 %u B，只解析前 %u B", (unsigned)n, (unsigned)n);

    parse(s_buf, n, &s_cfg);
    ESP_LOGI(TAG, "已加载 %s: %u B, crc32=%08x, types=%u, vars=%u",
             CFG_PATH, (unsigned)n, (unsigned)s_cfg.crc32,
             s_cfg.type_count, s_cfg.var_count);
    return ESP_OK;
}

const cfg_t *cfg_get(void) { return &s_cfg; }

int cfg_type_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < s_cfg.type_count; i++) {
        if (strncmp(s_cfg.types[i], name, CFG_NAME_LEN) == 0) return i;
    }
    return -1;
}

esp_err_t cfg_validate_vars(const uint8_t *vars, int n)
{
    if (!vars || n < 0 || n > s_cfg.var_count) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < n; i++) {
        if (vars[i] > s_cfg.vars[i].max) {
            ESP_LOGE(TAG, "变量 %s=%u 超出上限 %u",
                     s_cfg.vars[i].name, vars[i], s_cfg.vars[i].max);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

void cfg_crc_hex(char *buf, size_t n)
{
    if (!buf || n < 9) { if (buf && n) buf[0] = '\0'; return; }
    snprintf(buf, n, "%08x", (unsigned)s_cfg.crc32);
}

void cfg_dump(void)
{
    ESP_LOGI(TAG, "---- 配置 (%s, crc=%08x) ----",
             s_cfg.from_file ? "文件" : "内置默认", (unsigned)s_cfg.crc32);
    for (int i = 0; i < s_cfg.type_count; i++) {
        ESP_LOGI(TAG, "  type[%d] %s", i, s_cfg.types[i]);
    }
    for (int v = 0; v < s_cfg.var_count; v++) {
        const cfg_var_t *cv = &s_cfg.vars[v];
        char line[160];
        int off = snprintf(line, sizeof(line), "  var[%d] %s max=%u:", v, cv->name, cv->max);
        for (int i = 1; i <= cv->max && off < (int)sizeof(line) - 24; i++) {
            off += snprintf(line + off, sizeof(line) - off, " %d=%s",
                            i, cv->has_label[i] ? cv->label[i] : "-");
        }
        ESP_LOGI(TAG, "%s", line);
    }
}
