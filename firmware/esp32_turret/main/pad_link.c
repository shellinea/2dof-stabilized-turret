/*
 * SPDX-License-Identifier: Apache-2.0
 * M2b：GAP 层 —— 扫描、连接、断开后自动重扫。
 *
 * ⚠ 失效判据挂在「连接状态」上，不挂在「数据流」上：
 * 手柄静置时一包不发是正常的，只有 GAP 的 DISCONNECT 才算真掉线，
 * 也只有那时才把手柄状态压回中位（误停比不响应危险得多）。
 */
#include <stdio.h>
#include <assert.h>

#include "host/ble_hs.h"
#include "esp_central.h"        /* peer_add / peer_delete / print_* */

#include "codexpad.h"

/* 本机扫描/连接用的地址类型，以及当前连接状态 */
static uint8_t  g_own_addr_type;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int      g_connecting;

static int pad_gap_event(struct ble_gap_event *event, void *arg);

/* 发起连接。已在连接中或已连上时什么都不做。 */
int pad_link_connect(const ble_addr_t *addr)
{
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE || g_connecting) return 0;

    g_connecting = 1;

    /* 发起连接前必须停掉扫描（NimBLE 不允许边扫边连）*/
    ble_gap_disc_cancel();

    printf("\n[M2b] 发起连接 → %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr->val[5], addr->val[4], addr->val[3],
           addr->val[2], addr->val[1], addr->val[0]);

    int rc = ble_gap_connect(g_own_addr_type, addr, 30000, NULL,
                             pad_gap_event, NULL);
    if (rc != 0) {
        printf("[M2b] 连接发起失败 rc=%d，恢复扫描\n", rc);
        g_connecting = 0;
        pad_link_scan();
    }
    return 0;
}

/* 开始（或重新开始）扫描。 */
void pad_link_scan(void)
{
    struct ble_gap_disc_params disc_params = {0};
    uint8_t own_addr_type;

    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        printf("[M2b] 推断本机地址类型失败 rc=%d\n", rc);
        return;
    }
    g_own_addr_type = own_addr_type;   /* 发起连接时要用同一个 */

    /* ⚠ 必须为 0：手柄靠「重复广播」持续上报按键状态，
     * 过滤掉重复就只看得到第一帧，按什么键都不会变了（执行文档 §4.2）。
     */
    disc_params.filter_duplicates = 0;

    /* ⚠ 必须为 0（主动扫描）：只被动扫描的话不会发 SCAN_REQ，
     * 就拿不到 SCAN_RSP —— 而手柄的厂商数据（按键状态）在扫描响应里。
     * blecent 原版这里写的是 1，照抄会把这一步卡死（执行文档 §4.2）。
     */
    disc_params.passive = 0;

    /* 其余参数用默认值 */
    disc_params.itvl          = 0;
    disc_params.window        = 0;
    disc_params.filter_policy = 0;
    disc_params.limited       = 0;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      pad_gap_event, NULL);
    if (rc != 0) {
        printf("[M2b] 启动扫描失败 rc=%d\n", rc);
    }
}

static void on_connect(struct ble_gap_event *event)
{
    struct ble_gap_conn_desc desc;

    if (event->connect.status != 0) {
        printf("[M2b] 连接失败 status=%d，恢复扫描\n", event->connect.status);
        g_connecting = 0;
        pad_link_scan();
        return;
    }

    int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
    assert(rc == 0);
    print_conn_desc(&desc);

    /* 记住这个 peer（blecent 的连接管理表，断开时要对称地删掉）*/
    rc = peer_add(event->connect.conn_handle);
    if (rc != 0) {
        printf("[M2b] peer_add 失败 rc=%d\n", rc);
        return;
    }

    g_conn_handle = event->connect.conn_handle;
    g_connecting  = 0;

    /* 连上就开始跑自己的发现链：0xFFA0 → 0xFFA1 → CCCD → 订阅 */
    pad_gatt_start(g_conn_handle);
}

static void on_disconnect(struct ble_gap_event *event)
{
    printf("[M2b] 断开 reason=%d\n", event->disconnect.reason);
    print_conn_desc(&event->disconnect.conn);

    peer_delete(event->disconnect.conn.conn_handle);

    /* 清掉连接状态，下面的 pad_link_scan() 会重新扫到并自动重连（AC-BLE-5）*/
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_connecting  = 0;
    pad_gatt_reset();

    /* M2c：**唯一**的清零点。真掉线 → 手柄状态压回中位，ω_ref 立刻归零（AC-BLE-8）。
     * 注意这里不是「超时清零」：手柄静置不发包是正常的，不受影响。 */
    pad_input_center();

    /* 恢复扫描 */
    pad_link_scan();
}

/* NimBLE 把所有 GAP 事件都送到这一个回调里。 */
static int pad_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields fields;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) {
            return 0;
        }
        print_adv_fields(&fields);

        /* 诊断：统计各事件类型的占比。只要出现过 0x04，
         * 就说明主动扫描生效、SCAN_RSP 能收到。 */
        static unsigned evt_hist[8], evt_total;
        if (event->disc.event_type < 8) {
            evt_hist[event->disc.event_type]++;
        }
        if (++evt_total % 200 == 0) {
            printf("[evt 直方图] 共 %u 包：", evt_total);
            for (int i = 0; i < 8; ++i) {
                if (evt_hist[i]) printf(" 0x%02X=%u", i, evt_hist[i]);
            }
            printf("\n");
        }

        /* M2a：认出 CodexPad 就解析广播里的按键位 */
        pad_scan_on_disc(&fields, &event->disc);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        on_connect(event);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        on_disconnect(event);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        printf("[M2b] 扫描结束 reason=%d\n", event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t  buf[64];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        pad_input_on_notify(buf, len);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        printf("[M2b] MTU 更新 conn_handle=%d cid=%d mtu=%d\n",
               event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 已经和这个设备有绑定，但它要重新配对。这里牺牲安全性换便利：
         * 直接扔掉旧绑定、接受新链路。 */
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}
