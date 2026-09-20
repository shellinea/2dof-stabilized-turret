/*
 * SPDX-License-Identifier: Apache-2.0
 * M2a：扫描 CodexPad-S10 广播并解析按键状态（这一步故意只扫不连）。
 *
 * 手柄的按键状态在 SCAN_RSP 的厂商数据里，ADV_IND 里没有 ——
 * 所以扫描必须主动（passive=0，否则不发 SCAN_REQ 就拿不到扫描响应），
 * 且不能过滤重复包（手柄靠重复广播持续上报，去重后按什么键都不会变）。
 * 详见 docs/03-BLE手柄接入执行文档.md §4.2 / 附录 A.5。
 */
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "host/ble_hs.h"

#include "codexpad.h"

/* 广播 Manufacturer Specific Data 布局（附录 A.5）：
 *   [0..1] company_id=0xFFFF   [2..9] "CodexPad"   [10..12] fw maj/min/patch
 *   [13..16] button_state(u32 LE)   [17] 已保持秒数        合计 18 字节 */
#define CODEXPAD_MFG_MIN_LEN  (2 + 8 + 3 + 4 + 1)

static void print_buttons(uint32_t m)
{
    printf("    buttons=0x%05lX [", (unsigned long)m);
    for (size_t i = 0; i < k_btn_names_n; ++i) {
        if (m & k_btn_names[i].bit) {
            printf(" %s", k_btn_names[i].name);
        }
    }
    printf(" ]\n");
}

/* 把一包广播按 AD 结构逐个摊开： [类型:内容]
 * 用来确认厂商数据到底在 ADV_IND 还是 SCAN_RSP 里（evt 字段见 ble_gap.h）*/
static void dump_adv(const struct ble_gap_disc_desc *disc)
{
    printf("      evt=0x%02X len=%u:", disc->event_type, disc->length_data);
    int i = 0;
    while (i < disc->length_data) {
        uint8_t len = disc->data[i];
        if (len == 0 || i + 1 + len > disc->length_data) {
            printf(" <坏长度 %u>", len);
            break;
        }
        printf(" [%02X:", disc->data[i + 1]);
        for (int j = 0; j < len - 1; ++j) {
            printf("%02X", disc->data[i + 2 + j]);
        }
        printf("]");
        i += 1 + len;
    }
    printf("\n");
}

/* 手柄地址：第一次靠名字认出来之后记住它。
 * 之后连「没有 name 字段的扫描响应」也能认出来 —— 只按名字认会漏掉那类包，
 * 这正是上一轮误判「手柄不回扫描请求」的原因。 */
static ble_addr_t g_pad_addr;
static int        g_pad_addr_ok;

static int is_pad_addr(const ble_addr_t *a)
{
    return g_pad_addr_ok && a->type == g_pad_addr.type &&
           memcmp(a->val, g_pad_addr.val, 6) == 0;
}

void pad_scan_on_disc(const struct ble_hs_adv_fields *f,
                      const struct ble_gap_disc_desc *disc)
{
    int named = (f->name != NULL && f->name_len >= 8 &&
                 memcmp(f->name, "CodexPad", 8) == 0);

    if (named && !g_pad_addr_ok) {
        g_pad_addr = disc->addr;
        g_pad_addr_ok = 1;
        printf("\n[认出手柄地址] addr=%02X:%02X:%02X:%02X:%02X:%02X type=%u\n",
               disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
               disc->addr.val[2], disc->addr.val[1], disc->addr.val[0],
               disc->addr.type);
    }

    if (!named && !is_pad_addr(&disc->addr)) {
        /* 不是手柄：每个不同的名字只报一次，用来确认板子蓝牙确实在工作 */
        if (f->name != NULL) {
            static char seen[12][24];
            static int  n_seen;
            for (int i = 0; i < n_seen; ++i) {
                if ((int)strlen(seen[i]) == f->name_len &&
                    memcmp(seen[i], f->name, f->name_len) == 0) {
                    return;
                }
            }
            if (n_seen < 12 && f->name_len < 24) {
                memcpy(seen[n_seen], f->name, f->name_len);
                seen[n_seen][f->name_len] = '\0';
                n_seen++;
            }
            printf("    ... %.*s  rssi=%d\n", f->name_len, f->name, disc->rssi);
        }
        return;
    }

    /* 速率统计：必须在去重之前数，否则重复帧全被吃掉、看不出真实频率。
     * 按事件类型分开数 —— 手柄每个广播事件会给我们两帧
     * （ADV_IND + SCAN_RSP），所以按键真实更新率要看 0x00 那一列，
     * 不能拿两列相加。 */
    static unsigned pad_frames[8];
    static unsigned pad_total;
    static int64_t  pad_t0;
    if (pad_t0 == 0) {
        pad_t0 = esp_timer_get_time();
    }
    if (disc->event_type < 8) {
        pad_frames[disc->event_type]++;
    }
    if (++pad_total % 200 == 0) {
        int64_t ms = (esp_timer_get_time() - pad_t0) / 1000;
        printf("[手柄速率] 累计 %u 帧 / %d ms:", pad_total, (int)ms);
        for (int i = 0; i < 8; ++i) {
            if (pad_frames[i]) {
                printf(" 0x%02X=%u(%.1fHz)", i, pad_frames[i],
                       1000.0 * pad_frames[i] / (double)ms);
            }
        }
        printf("\n");
    }

    /* 手柄。按 (事件类型 + 内容) 去重：手柄靠重复广播不停上报，
     * 同样内容会刷几千遍；只有内容变了才值得打一行。 */
    static unsigned long last_hash[8];
    static int           last_set[8];
    unsigned long h = 0;
    for (int i = 0; i < disc->length_data; ++i) {
        h = h * 131u + disc->data[i];
    }
    unsigned ev = disc->event_type;
    if (ev < 8 && last_set[ev] && last_hash[ev] == h) {
        return;
    }
    if (ev < 8) { last_set[ev] = 1; last_hash[ev] = h; }

    printf(">>> %s evt=0x%02X  RSSI=%d dBm  addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
           named ? "发现手柄" : "手柄(无名字包)", disc->event_type, disc->rssi,
           disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
           disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
    dump_adv(disc);

    /* M2b：认识手柄了，发起连接（连接后就不再扫描）。*/
    pad_link_connect(&disc->addr);

    const uint8_t *m = f->mfg_data;
    if (m == NULL || f->mfg_data_len < CODEXPAD_MFG_MIN_LEN ||
        m[0] != 0xFF || m[1] != 0xFF || memcmp(&m[2], "CodexPad", 8) != 0) {
        printf("      (无 Manufacturer Specific Data，或格式不符)"
               " name_len=%u mfg_len=%u\n", f->name_len, f->mfg_data_len);
        return;
    }

    uint32_t btn = (uint32_t)m[13] | ((uint32_t)m[14] << 8) |
                   ((uint32_t)m[15] << 16) | ((uint32_t)m[16] << 24);
    printf("      fw=%u.%u.%u  held=%us\n", m[10], m[11], m[12], m[17]);
    print_buttons(btn);
}
