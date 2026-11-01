/* sd_index —— project/directory.txt 的唯一读写入口
 *
 * 设计要点（见 docs/SD目录结构与数据管理.md）：
 *   - 开机读一次索引 + readdir 对账，之后列表全走内存，零 SD I/O
 *   - 采集完成只做"追加一行"（实测 5 ms，恒定）
 *   - 删除后才整写（实测 4,500 组约 0.74 s，必须异步调用）
 *   - 编号单调：next_id = max(头部 next_id, 现存最大编号+1)，删除不插补、不复用
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_INDEX_ROOT        "/sd/project"
#define SD_INDEX_FILE        "/sd/project/directory.txt"
#define SD_INDEX_TMP         "/sd/project/directory.txt.tmp"
#define SD_INDEX_CLIP_NAME   "clip.gseq"
#define SD_INDEX_MAX_GROUPS  8192

/* flags */
#define SD_GROUP_FLAG_OK      0x00
#define SD_GROUP_FLAG_BAD     0x01   /* 掉帧/校验失败 */
#define SD_GROUP_FLAG_ORPHAN  0x02   /* 开机对账时补录的孤儿目录，元数据不全 */

typedef struct {
    uint32_t id;            /* data0007 -> 7 */
    char     name[16];      /* "data0007" */
    char     label[16];     /* 类别名（来自 config.json 的 types） */
    uint8_t  vars[4];       /* 4 个变量值（数字编码，0 = 未设置） */
    uint16_t frames;
    uint32_t bytes;
    uint32_t mtime;
    uint8_t  flags;
} sd_group_t;

/** 开机：挂载后调用。读索引 → readdir 对账 → 计算 next_id。约 0.5 s @4,500 组 */
esp_err_t sd_index_init(void);

int                     sd_index_count(void);
const sd_group_t       *sd_index_get(int idx);
const sd_group_t       *sd_index_find(const char *name);

/** 当前可用的下一个编号（不占用） */
uint32_t                sd_index_next_id(void);

/** 占用一个编号（next_id++），用于新建组目录前 */
uint32_t                sd_index_claim_id(void);

/** 采集完成：追加一行（5 ms）。调用方需已把 clip.gseq 落盘 */
esp_err_t sd_index_append(const sd_group_t *g);

/** 从内存列表移除（不写盘）；删除流程随后应调用 sd_index_rewrite() */
esp_err_t sd_index_remove(const char *name);

/** 整写索引：写 tmp → 校验 → rename，并刷新头部 next_id */
esp_err_t sd_index_rewrite(void);

/** 重新对账（readdir + 索引），返回处理掉的异常数 */
int       sd_index_rescan(void);

/** 打印统计（调试用） */
void      sd_index_dump(int max_lines);
