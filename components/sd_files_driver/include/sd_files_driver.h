#ifndef SD_FILES_DRIVER_H
#define SD_FILES_DRIVER_H

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief microSD 卡挂载点（VFS 前缀）
 */
#define SD_MOUNT_POINT  "/sd"

/**
 * @brief SDMMC 1-bit 总线引脚（对应原理图 U7：CLK/CMD/DAT0）
 *        注意：GPIO35/36/37 已被 N16R8 的 Octal PSRAM 占用，不可改用。
 */
#define SD_CLK_PIN      39
#define SD_CMD_PIN      38
#define SD_DAT0_PIN     40

/**
 * @brief 初始化 SDMMC 总线并挂载 FATFS 到 SD_MOUNT_POINT
 *
 * - 1-bit 总线、20 MHz 初速；实际工作频率由驱动按卡能力协商，不在此强制。
 * - 挂载失败不会自动格式化（format_if_mount_failed = false）。
 * - 重复调用是安全的：已挂载时直接返回 ESP_OK。
 *
 * @return ESP_OK 成功；其余为 esp_err_t 错误码（如 ESP_FAIL 无卡、ESP_ERR_INVALID_STATE 等）
 */
esp_err_t sd_files_driver_init(void);

/**
 * @brief 卸载 FATFS 并释放 SDMMC 总线
 *
 * 未挂载时调用返回 ESP_OK（幂等）。
 */
esp_err_t sd_files_driver_deinit(void);

/**
 * @brief 当前是否已挂载
 */
bool sd_files_driver_is_mounted(void);

/**
 * @brief 卸载后重新挂载（本板无 CD 脚，插拔卡后由调用方轮询此接口恢复）
 *
 * @return ESP_OK 表示重挂成功
 */
esp_err_t sd_files_driver_remount(void);

/**
 * @brief 取挂载点字符串（即 SD_MOUNT_POINT）
 */
const char *sd_files_driver_mount_point(void);

/**
 * @brief 查询卡容量（字节）
 *
 * @param[out] total_bytes 总容量，可为 NULL
 * @param[out] free_bytes  可用容量，可为 NULL
 * @return ESP_OK 成功；未挂载返回 ESP_ERR_INVALID_STATE
 */
esp_err_t sd_files_driver_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif

#endif /* SD_FILES_DRIVER_H */
