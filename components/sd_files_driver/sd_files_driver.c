#include "sd_files_driver.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "SD_FILES";

/* 由 FATFS 挂载时填充，卸载时清空；本文件外不可见 */
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static esp_err_t sd_mount(void)
{
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 绝不自动格式化用户卡 */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* 上限给到 40 MHz（SD High Speed）：驱动会先以 20 MHz 读取 CSD/CMD6，
       卡支持 HS 则切到 40 MHz；不支持时由 sdmmc_common.c 自动退回 20 MHz。
       注意：UHS-I（SDR50/SDR104，50/100 MHz）需要 1.8 V 信令 + 4-bit 总线，
       本板固定 3.3 V + 1-bit，因此 40 MHz 就是硬件天花板。
       若实测 40 MHz 不稳，把下面一行改回 SDMMC_FREQ_DEFAULT 即可回退。 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                        /* 1-bit 总线 */
    slot.clk   = SD_CLK_PIN;
    slot.cmd   = SD_CMD_PIN;
    slot.d0    = SD_DAT0_PIN;
    /* 板上有 R16/R17/R18 10K 外部上拉，内部上拉仅作补充，无害 */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        ESP_LOGE(TAG, "挂载 %s 失败: %s", SD_MOUNT_POINT, esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "挂载成功: %s", SD_MOUNT_POINT);
    if (s_card != NULL) {
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGI(TAG, "总线频率: 实际 %u kHz / 协商上限 %u kHz",
                 (unsigned)s_card->real_freq_khz, (unsigned)s_card->max_freq_khz);
        if (s_card->real_freq_khz < SDMMC_FREQ_HIGHSPEED) {
            ESP_LOGW(TAG, "未进入 High Speed（卡不支持或协商回退），运行在 %u kHz",
                     (unsigned)s_card->real_freq_khz);
        }
    }
    return ESP_OK;
}

esp_err_t sd_files_driver_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "已挂载，忽略重复初始化");
        return ESP_OK;
    }
    return sd_mount();
}

esp_err_t sd_files_driver_deinit(void)
{
    if (!s_mounted || s_card == NULL) {
        s_mounted = false;
        s_card = NULL;
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "卸载失败: %s", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "已卸载 %s", SD_MOUNT_POINT);
    return ESP_OK;
}

bool sd_files_driver_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_files_driver_remount(void)
{
    esp_err_t err = sd_files_driver_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重挂前卸载失败(%s)，继续尝试挂载", esp_err_to_name(err));
    }
    return sd_mount();
}

const char *sd_files_driver_mount_point(void)
{
    return SD_MOUNT_POINT;
}

esp_err_t sd_files_driver_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);
}
