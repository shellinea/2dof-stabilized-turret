/*
 * SPDX-License-Identifier: Apache-2.0
 * 应用入口：拉起 NimBLE 协议栈，接上手柄输入模块。
 *
 * 手柄链路的实现拆在 pad_*.c，公共接口见 codexpad.h。
 */
#include <assert.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_central.h"        /* peer_init / ble_store_util_status_rr */

#include "codexpad.h"

static const char *TAG = "turret_ble";

/* 实现在 NimBLE 的 ble_hs_pvcy.c，没随公共头导出，这里自行声明 */
void ble_store_config_init(void);

static void pad_on_reset(int reason)
{
    ESP_LOGE(TAG, "协议栈复位 reason=%d", reason);
}

static void pad_on_sync(void)
{
    /* 确保本机有可用 BLE 地址（优先公有地址）*/
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    pad_link_scan();
}

static void pad_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host 任务已启动");
    /* 只在 nimble_port_stop() 执行后才返回 */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

void app_main(void)
{
    /* NVS 用来存射频校准数据 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %d", ret);
        return;
    }

    /* 先让控制环跑起来：第一个 notify 到达前队列里就有中位值可读 */
    pad_input_init();

    ble_hs_cfg.reset_cb        = pad_on_reset;
    ble_hs_cfg.sync_cb         = pad_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 64, 64, 64);
    assert(rc == 0);

    rc = ble_svc_gap_device_name_set("esp32-turret");
    assert(rc == 0);

    ble_store_config_init();

    nimble_port_freertos_init(pad_host_task);
}
