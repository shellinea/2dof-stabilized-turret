/*
 * SPDX-License-Identifier: Apache-2.0
 * 应用入口：拉起 NimBLE 协议栈，接上手柄输入模块与电机总线。
 *
 * 手柄链路的实现拆在 pad_*.c，公共接口见 codexpad.h；
 * 电机（TWAI + Emm + 差速逆解）在 motor_twai.c，接口见 motor_twai.h。
 */
#include <assert.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_central.h"        /* peer_init / ble_store_util_status_rr */

#include "codexpad.h"
#include "motor_twai.h"

static const char *TAG = "turret_ble";

/* 实现在 NimBLE 的 ble_hs_pvcy.c，没随公共头导出，这里自行声明 */
void ble_store_config_init(void);

/* 千分之一度 -> "±D.DDD"，好塞进 printf。
 * 刻意不用 %f：本 IDF 若开了 newlib-nano 格式化，%f 会打印不出来。
 * 6 格轮转缓冲 —— 一条 printf 里要塞好几个值，共用一格会被后一个冲掉前一个。 */
static const char *fmt_deg(int32_t mdeg)
{
    static char b[6][24];
    static unsigned i;
    char *p = b[i++ % 6u];
    const int32_t a = (mdeg < 0) ? -mdeg : mdeg;
    snprintf(p, sizeof(b[0]), "%s%d.%03d", mdeg < 0 ? "-" : "+",
             (int)(a / 1000), (int)(a % 1000));
    return p;
}

/* 上电自检：两台电机探活 + 读一次姿态 + 使能。
 * 必须在控制环（pad_input_init）之前 —— 控制环一跑就开始发速度指令。 */
static void motor_bringup(void)
{
    if (!motor_init()) {
        return;                     /* TWAI 没建起来，后面 motor_ready() 全是 false */
    }

    /* 两台分别探一次：读不到是哪台的问题，日志里要说清楚，不然只看到
     * "读不到位置"没法下手。上电没使能也能读 36，所以这里读不到纯粹是链路问题。 */
    int32_t p1 = 0, p2 = 0;
    const bool has1 = motor_read_pos(1, &p1);
    const bool has2 = motor_read_pos(2, &p2);
    printf("[motor] 1 号 %s   2 号 %s\n",
           has1 ? fmt_deg(p1) : "读不到（查地址/接线）",
           has2 ? fmt_deg(p2) : "读不到（查地址/接线）");

    if (has1 && has2) {
        int32_t tilt = 0, pan = 0;
        if (motor_read_pose(&tilt, &pan)) {
            printf("[motor] 初始姿态: 俯仰=%s 度  自转=%s 度\n",
                   fmt_deg(tilt), fmt_deg(pan));
        }
    }

    /* 使能是必须的：掉电再上电后电机回到失能态，不使能的话速度指令发出去
     * 也没人动，症状是"日志正常但转台不动"。 */
    if (!motor_enable(true)) {
        printf("[motor] ⚠ 使能失败\n");
    }
}

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

    /* ---- M2e：电机总线上电自检 ----
     * 必须在控制环（pad_input_init）之前 —— 控制环一跑就开始发速度指令，
     * 那会儿 TWAI 必须已经就绪，不然指令全被 motor_ready() 挡掉。 */
    motor_bringup();

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
