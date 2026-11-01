/* cfg_store —— /sd/project/config.json 的唯一解析入口
 *
 * 设计（见 docs/配置化标签执行计划.md）：
 *   - 标准 JSON，用 cJSON 解析，**仅开机一次**（可显式 reload）
 *   - 变量取值只存 uint8_t 数字；labels 只用于显示与 PC 端还原
 *   - CRC32 必须与 zlib 兼容，否则与 PC 侧写的 # cfg_crc= 对不上
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CFG_PATH          "/sd/project/config.json"
#define CFG_MAX_TYPES     32
#define CFG_MAX_VARS      4
#define CFG_MAX_LABEL     32     /* 单个变量的取值上限（max 的取值范围） */
#define CFG_NAME_LEN      16

typedef struct {
    char     name[CFG_NAME_LEN];
    uint8_t  max;                          /* 1..CFG_MAX_LABEL */
    bool     has_label[CFG_MAX_LABEL + 1]; /* 下标 = 数字编码 */
    char     label[CFG_MAX_LABEL + 1][CFG_NAME_LEN];
} cfg_var_t;

typedef struct {
    char      types[CFG_MAX_TYPES][CFG_NAME_LEN];
    uint8_t   type_count;
    cfg_var_t vars[CFG_MAX_VARS];
    uint8_t   var_count;
    uint32_t  crc32;                       /* 0 = 内置默认（文件缺失或解析失败） */
    bool      from_file;
} cfg_t;

/** 读并解析 config.json；失败时回落到内置默认（仍返回 ESP_OK） */
esp_err_t cfg_load(void);

/** 只读访问当前配置 */
const cfg_t *cfg_get(void);

/** 类别名 → 类别下标；-1 表示不存在 */
int cfg_type_index(const char *name);

/** 校验一组变量值是否合法（每项 0..max）；返回 ESP_OK 或 ESP_ERR_INVALID_ARG */
esp_err_t cfg_validate_vars(const uint8_t *vars, int n);

/** 把 crc32 格式化成 8 位十六进制 */
void cfg_crc_hex(char *buf, size_t n);

/** 打印当前配置（调试用） */
void cfg_dump(void);
