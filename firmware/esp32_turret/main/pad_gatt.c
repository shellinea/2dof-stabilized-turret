/*
 * SPDX-License-Identifier: Apache-2.0
 * M2b：GATT 发现链 —— 服务 0xFFA0 → 特征 0xFFA1 → CCCD 0x2902 → 订阅 notify。
 *
 * 走的是自己这条链，不是 blecent 示例那套通用的 ANS 演示流程：
 * 需要的不只是「发现」，还要把 0xFFA1 的特征值句柄和它自己的 CCCD 句柄记下来，
 * 才能写 CCCD 打开 notify。顺路把整个 GATT 表打出来（含文档没写的 0xFFE0）。
 */
#include <stdio.h>

#include "host/ble_hs.h"

#include "codexpad.h"

#define MAX_SVCS 16

typedef struct { uint16_t start, end; } svc_range_t;

static svc_range_t g_svcs[MAX_SVCS];
static int      g_svc_n, g_svc_i;
static uint16_t g_chr_val_handle;      /* 0xFFA1 的特征值句柄 */
static uint16_t g_chr_svc_end;         /* 0xFFA1 所在服务的上界 */
static uint16_t g_cccd_handle;

static void disc_next_svc(uint16_t conn_handle);

static int on_cccd_write(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    printf("[M2b] 订阅写回 status=%d attr_handle=%u\n",
           error->status, attr->handle);
    return 0;
}

static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0) {
        printf("[GATT]     描述符 uuid=0x%04X handle=%u（属于 val_handle=%u）\n",
               ble_uuid_u16(&dsc->uuid.u), dsc->handle, chr_val_handle);
        if (ble_uuid_u16(&dsc->uuid.u) == CODEXPAD_UUID_CCCD &&
            !g_cccd_handle) {
            g_cccd_handle = dsc->handle;   /* 取第一个，就是 0xFFA1 自己的 */
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (g_cccd_handle) {
            uint8_t on[2] = { 0x01, 0x00 };      /* notify 使能 */
            int rc = ble_gattc_write_flat(conn_handle, g_cccd_handle,
                                          on, sizeof(on), on_cccd_write, NULL);
            printf("[M2b] 订阅 notify：CCCD handle=%u rc=%d\n", g_cccd_handle, rc);
        } else {
            printf("[M2b] ⚠ 0xFFA1 下没找到 CCCD(0x2902)，没法订阅\n");
        }
    }
    return 0;
}

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0) {
        printf("[GATT]   特征 uuid=0x%04X def=%u val=%u props=0x%02X%s\n",
               ble_uuid_u16(&chr->uuid.u), chr->def_handle, chr->val_handle,
               chr->properties,
               (chr->properties & 0x10) ? " (notify)" : "");
        if (ble_uuid_u16(&chr->uuid.u) == CODEXPAD_UUID_CHR) {
            g_chr_val_handle = chr->val_handle;
            g_chr_svc_end    = g_svcs[g_svc_i].end;
        }
    } else if (error->status == BLE_HS_EDONE) {
        g_svc_i++;
        disc_next_svc(conn_handle);
    }
    return 0;
}

static void disc_next_svc(uint16_t conn_handle)
{
    while (g_svc_i < g_svc_n) {
        svc_range_t s = g_svcs[g_svc_i];
        if (s.start == 0) { g_svc_i++; continue; }
        ble_gattc_disc_all_chrs(conn_handle, s.start, s.end, on_chr_disc, NULL);
        return;
    }
    /* 所有服务的特征都打完了 → 去找 0xFFA1 的 CCCD 并订阅 */
    if (g_chr_val_handle) {
        printf("[M2b] 找到输入特征 0xFFA1（val_handle=%u），往下找 CCCD\n",
               g_chr_val_handle);
        ble_gattc_disc_all_dscs(conn_handle, g_chr_val_handle,
                                g_chr_svc_end, on_dsc_disc, NULL);
    } else {
        printf("[M2b] ⚠ 整个 GATT 表里都没找到特征 0xFFA1\n");
    }
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0) {
        uint16_t u = ble_uuid_u16(&svc->uuid.u);
        if (u != 0) {
            printf("[GATT] 服务 uuid=0x%04X handle=%u..%u\n",
                   u, svc->start_handle, svc->end_handle);
        } else {
            printf("[GATT] 服务 (128 位 UUID) handle=%u..%u\n",
                   svc->start_handle, svc->end_handle);
        }
        if (g_svc_n < MAX_SVCS) {
            g_svcs[g_svc_n].start = svc->start_handle;
            g_svcs[g_svc_n].end   = svc->end_handle;
            g_svc_n++;
        }
    } else if (error->status == BLE_HS_EDONE) {
        printf("[M2b] 共 %d 个服务，逐个翻特征\n", g_svc_n);
        g_svc_i = 0;
        disc_next_svc(conn_handle);
    }
    return 0;
}

void pad_gatt_start(uint16_t conn_handle)
{
    pad_gatt_reset();
    int rc = ble_gattc_disc_all_svcs(conn_handle, on_svc_disc, NULL);
    if (rc != 0) {
        printf("[M2b] 发现服务失败 rc=%d\n", rc);
    }
}

void pad_gatt_reset(void)
{
    g_svc_n = g_svc_i = 0;
    g_chr_val_handle = 0;
    g_chr_svc_end    = 0;
    g_cccd_handle    = 0;
}
