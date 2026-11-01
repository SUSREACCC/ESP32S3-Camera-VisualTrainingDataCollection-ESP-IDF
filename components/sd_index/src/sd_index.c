#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "cfg_store.h"

#include "sd_index.h"

static const char *TAG = "SDIDX";

static sd_group_t *s_groups;            /* PSRAM */
static int         s_count;
static uint32_t    s_next_id = 1;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static uint32_t parse_id(const char *name)
{
    if (strncmp(name, "data", 4) != 0) return 0;
    const char *p = name + 4;
    if (*p < '0' || *p > '9') return 0;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > 9999999) return 0;
        p++;
    }
    return (*p == '\0') ? v : 0;
}

static bool dir_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* 组内 clip.gseq 的字节数（不打开文件，只用 stat） */
static uint32_t clip_size(const char *name)
{
    char path[320];
    snprintf(path, sizeof(path), SD_INDEX_ROOT "/%s/" SD_INDEX_CLIP_NAME, name);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint32_t)st.st_size;
}

/* ------------------------------------------------------------------ */
/* 解析一行：name|subject|label|light|bg|dist|angle|hand|frames|bytes|mtime|flags */
static bool parse_line(char *line, sd_group_t *g)
{
    memset(g, 0, sizeof(*g));

    /* 手工按 '|' 切分：不能用 strtok（会把连续分隔符当一个，空字段导致错位） */
    char *f[12];
    int   nf = 0;
    f[nf++] = line;
    for (char *q = line; *q; q++) {
        if (*q == '|') {
            *q = '\0';
            if (nf < 12) f[nf++] = q + 1;
        }
    }
    if (nf < 7) return false;                 /* 至少 name|label|v0|v1|v2|v3|frames */

    strncpy(g->name, f[0], sizeof(g->name) - 1);
    g->id = parse_id(g->name);
    if (g->id == 0) return false;

    strncpy(g->label, f[1], sizeof(g->label) - 1);
    g->vars[0] = (uint8_t)atoi(f[2]);
    g->vars[1] = (uint8_t)atoi(f[3]);
    g->vars[2] = (uint8_t)atoi(f[4]);
    g->vars[3] = (uint8_t)atoi(f[5]);
    g->frames  = (uint16_t)atoi(f[6]);
    if (nf > 7)  g->bytes = (uint32_t)strtoul(f[7], NULL, 10);
    if (nf > 8)  g->mtime = (uint32_t)strtoul(f[8], NULL, 10);
    if (nf > 9)  g->flags = (uint8_t)atoi(f[9]);
    return true;
}

static int add_group(const sd_group_t *g)
{
    if (s_count >= SD_INDEX_MAX_GROUPS) return -1;
    s_groups[s_count++] = *g;
    return s_count - 1;
}

int sd_index_count(void) { return s_count; }

const sd_group_t *sd_index_get(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_groups[idx] : NULL;
}

const sd_group_t *sd_index_find(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_groups[i].name, name) == 0) return &s_groups[i];
    }
    return NULL;
}

uint32_t sd_index_next_id(void) { return s_next_id; }

uint32_t sd_index_claim_id(void)
{
    lock();
    uint32_t id = s_next_id++;
    unlock();
    return id;
}

/* ------------------------------------------------------------------ */
esp_err_t sd_index_init(void)
{
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();

    if (s_groups == NULL) {
        s_groups = heap_caps_calloc(SD_INDEX_MAX_GROUPS, sizeof(sd_group_t), MALLOC_CAP_SPIRAM);
        if (!s_groups) {
            s_groups = heap_caps_calloc(SD_INDEX_MAX_GROUPS, sizeof(sd_group_t), MALLOC_CAP_INTERNAL);
        }
        if (!s_groups) {
            ESP_LOGE(TAG, "索引内存分配失败（需要 %u KB）",
                     (unsigned)(SD_INDEX_MAX_GROUPS * sizeof(sd_group_t) / 1024));
            return ESP_ERR_NO_MEM;
        }
    }

    mkdir(SD_INDEX_ROOT, 0775);
    s_count = 0;

    uint32_t header_next = 0;
    int      lines = 0;

    /* ---- 1) 读索引 ---- */
    FILE *f = fopen(SD_INDEX_FILE, "r");
    if (f) {
        static char line[192];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ') p++;
            if (*p == '#') {
                unsigned long v = 0;
                if (sscanf(p, "# next_id=%lu", &v) == 1 && (uint32_t)v > header_next) {
                    header_next = (uint32_t)v;
                }
                continue;
            }
            if (*p == '\0' || *p == '\n' || *p == '\r') continue;
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

            sd_group_t g;
            if (parse_line(p, &g)) {
                add_group(&g);
                lines++;
            } else {
                ESP_LOGW(TAG, "索引行解析失败: %s", p);
            }
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "索引不存在，将新建");
    }

    /* ---- 2) readdir 对账 ---- */
    int orphans = 0, stale = 0;
    uint32_t max_id = 0;

    DIR *d = opendir(SD_INDEX_ROOT);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        uint32_t id = parse_id(e->d_name);
        if (id == 0) continue;

        char path[320];
        snprintf(path, sizeof(path), SD_INDEX_ROOT "/%s", e->d_name);
        if (!dir_exists(path)) continue;
        if (id > max_id) max_id = id;

        bool found = false;
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_groups[i].name, e->d_name) == 0) { found = true; break; }
        }
        if (found) continue;

        /* 孤儿目录：补录（不打开文件，只用 stat） */
        sd_group_t g;
        memset(&g, 0, sizeof(g));
        strncpy(g.name, e->d_name, sizeof(g.name) - 1);
        g.id = id;
        g.bytes = clip_size(e->d_name);
        g.frames = 0;                 /* 不打开文件就无法得知 */
        g.flags = SD_GROUP_FLAG_ORPHAN;
        add_group(&g);
        orphans++;
        ESP_LOGW(TAG, "补录孤儿目录 %s（%u B，元数据不全）", g.name, (unsigned)g.bytes);
    }
    if (d) closedir(d);

    /* 索引里有但目录没了 → 删行 */
    for (int i = s_count - 1; i >= 0; i--) {
        char path[320];
        snprintf(path, sizeof(path), SD_INDEX_ROOT "/%s", s_groups[i].name);
        if (!dir_exists(path)) {
            ESP_LOGW(TAG, "删除失效索引行 %s（目录不存在）", s_groups[i].name);
            memmove(&s_groups[i], &s_groups[i + 1],
                    sizeof(sd_group_t) * (s_count - i - 1));
            s_count--;
            stale++;
        }
    }

    /* ---- 3) next_id：单调，删除不插补 ---- */
    uint32_t next = max_id + 1;
    if (header_next > next) next = header_next;
    s_next_id = next;

    ESP_LOGI(TAG, "索引就绪: %d 组（读入 %d 行），孤儿补录 %d，失效行清理 %d，next_id=%u",
             s_count, lines, orphans, stale, (unsigned)s_next_id);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
static size_t fmt_line(char *buf, size_t n, const sd_group_t *g)
{
    return (size_t)snprintf(buf, n,
        "%s|%s|%u|%u|%u|%u|%u|%u|%u|%u\n",
        g->name, g->label,
        g->vars[0], g->vars[1], g->vars[2], g->vars[3],
        g->frames, (unsigned)g->bytes, (unsigned)g->mtime, g->flags);
}

esp_err_t sd_index_append(const sd_group_t *g)
{
    if (!g) return ESP_ERR_INVALID_ARG;
    lock();

    bool exists = false;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_groups[i].name, g->name) == 0) { exists = true; break; }
    }
    if (!exists) add_group(g);

    FILE *f = fopen(SD_INDEX_FILE, "a");
    if (!f) {
        unlock();
        ESP_LOGE(TAG, "打开索引失败（追加）");
        return ESP_FAIL;
    }
    char line[192];
    size_t len = fmt_line(line, sizeof(line), g);
    size_t w = fwrite(line, 1, len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    unlock();
    if (w != len) return ESP_FAIL;
    ESP_LOGI(TAG, "索引追加 %s（%u B）", g->name, (unsigned)len);
    return ESP_OK;
}

esp_err_t sd_index_remove(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    lock();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_groups[i].name, name) == 0) {
            memmove(&s_groups[i], &s_groups[i + 1],
                    sizeof(sd_group_t) * (s_count - i - 1));
            s_count--;
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sd_index_rewrite(void)
{
    lock();
    FILE *f = fopen(SD_INDEX_TMP, "w");
    if (!f) {
        unlock();
        ESP_LOGE(TAG, "创建 tmp 索引失败");
        return ESP_FAIL;
    }

    char crchex[16];
    cfg_crc_hex(crchex, sizeof(crchex));
    fprintf(f, "# esp32cam dataset index v3\n");
    fprintf(f, "# next_id=%u\n", (unsigned)s_next_id);
    fprintf(f, "# cfg_crc=%s\n", crchex);
    fprintf(f, "# name|label|v0|v1|v2|v3|frames|bytes|mtime|flags\n");

    char line[192];
    for (int i = 0; i < s_count; i++) {
        size_t n = fmt_line(line, sizeof(line), &s_groups[i]);
        if (fwrite(line, 1, n, f) != n) {
            fclose(f);
            unlock();
            return ESP_FAIL;
        }
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    unlink(SD_INDEX_FILE ".bak");
    rename(SD_INDEX_FILE, SD_INDEX_FILE ".bak");
    if (rename(SD_INDEX_TMP, SD_INDEX_FILE) != 0) {
        unlock();
        ESP_LOGE(TAG, "rename 索引失败");
        return ESP_FAIL;
    }
    unlock();
    ESP_LOGI(TAG, "索引已重写: %d 组, next_id=%u", s_count, (unsigned)s_next_id);
    return ESP_OK;
}

int sd_index_rescan(void)
{
    return (sd_index_init() == ESP_OK) ? 0 : -1;
}

void sd_index_dump(int max_lines)
{
    ESP_LOGI(TAG, "---- 索引 %d 组, next_id=%u ----", s_count, (unsigned)s_next_id);
    int n = (max_lines > 0 && max_lines < s_count) ? max_lines : s_count;
    for (int i = 0; i < n; i++) {
        const sd_group_t *g = &s_groups[i];
        ESP_LOGI(TAG, "  %s %-10s vars=%u,%u,%u,%u %u帧 %uB flags=%u",
                 g->name, g->label, g->vars[0], g->vars[1], g->vars[2], g->vars[3],
                 g->frames, (unsigned)g->bytes, g->flags);
    }
}
