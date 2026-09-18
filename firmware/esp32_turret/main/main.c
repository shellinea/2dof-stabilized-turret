/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "blecent.h"
#if MYNEWT_VAL(BLE_GATT_CACHING)
#include "host/ble_esp_gattc_cache.h"
#endif

#if CONFIG_EXAMPLE_USE_CI_ADDRESS
#ifdef CONFIG_IDF_TARGET_ESP32
#define TEST_CI_ADDRESS_CHIP_OFFSET (0)
#elif CONFIG_IDF_TARGET_ESP32C2
#define TEST_CI_ADDRESS_CHIP_OFFSET (1)
#elif CONFIG_IDF_TARGET_ESP32C3
#define TEST_CI_ADDRESS_CHIP_OFFSET (2)
#elif CONFIG_IDF_TARGET_ESP32C6
#define TEST_CI_ADDRESS_CHIP_OFFSET (3)
#elif CONFIG_IDF_TARGET_ESP32C5
#define TEST_CI_ADDRESS_CHIP_OFFSET (4)
#elif CONFIG_IDF_TARGET_ESP32H2
#define TEST_CI_ADDRESS_CHIP_OFFSET (5)
#elif CONFIG_IDF_TARGET_ESP32P4
#define TEST_CI_ADDRESS_CHIP_OFFSET (6)
#elif CONFIG_IDF_TARGET_ESP32S3
#define TEST_CI_ADDRESS_CHIP_OFFSET (7)
#elif CONFIG_IDF_TARGET_ESP32C61
#define TEST_CI_ADDRESS_CHIP_OFFSET (8)
#endif
#endif

#if MYNEWT_VAL(BLE_GATTC)
/*** The UUID of the service containing the subscribable characteristic ***/
static const ble_uuid_t * remote_svc_uuid =
    BLE_UUID128_DECLARE(0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12,
                     	0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

/*** The UUID of the subscribable chatacteristic ***/
static const ble_uuid_t * remote_chr_uuid =
    BLE_UUID128_DECLARE(0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
                     	0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33);
#endif

static const char *tag = "NimBLE_BLE_CENT";
static int blecent_gap_event(struct ble_gap_event *event, void *arg);
static void blecent_scan(void);

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
static uint16_t cids[MYNEWT_VAL(BLE_EATT_CHAN_NUM)];
static uint16_t bearers;
#endif

void ble_store_config_init(void);

/* ===================== M2a：CodexPad-S10 广播观察 =====================
 * 见 docs/03-BLE手柄接入执行文档.md §4.2 / 附录 A.5
 * 只扫描不连接：手柄广播里就带按键状态，所以不连也能验协议理解对不对。
 * ==================================================================== */
#include <stdio.h>
#include <string.h>
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

/* 17 键位表（附录 A.4） */
static const struct { uint32_t bit; const char *name; } k_btn_names[] = {
    { 1u << 0,  "Up"    }, { 1u << 1,  "Down"  }, { 1u << 2,  "Left"  },
    { 1u << 3,  "Right" }, { 1u << 4,  "Sq/X"  }, { 1u << 5,  "Tr/Y"  },
    { 1u << 6,  "X/A"   }, { 1u << 7,  "Cir/B" }, { 1u << 8,  "L1"    },
    { 1u << 9,  "L2"    }, { 1u << 10, "L3"    }, { 1u << 11, "R1"    },
    { 1u << 12, "R2"    }, { 1u << 13, "R3"    }, { 1u << 14, "Select"},
    { 1u << 15, "Start" }, { 1u << 16, "Home"  },
};

static void print_buttons(uint32_t m)
{
    printf("    buttons=0x%05lX [", (unsigned long)m);
    for (size_t i = 0; i < sizeof(k_btn_names) / sizeof(k_btn_names[0]); ++i) {
        if (m & k_btn_names[i].bit) {
            printf(" %s", k_btn_names[i].name);
        }
    }
    printf(" ]\n");
}

/* 广播 Manufacturer Specific Data 布局（附录 A.5）：
 *   [0..1] company_id=0xFFFF   [2..9] "CodexPad"   [10..12] fw maj/min/patch
 *   [13..16] button_state(u32 LE)   [17] 已保持秒数        合计 18 字节 */
#define CODEXPAD_MFG_MIN_LEN  (2 + 8 + 3 + 4 + 1)

/* 把一包广播按 AD 结构逐个摊开： [类型:内容]
 * 用来确认厂商数据到底在 ADV_IND 还是 SCAN_RSP 里（evt 字段见 ble_gap.h） */
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
 * 之后连"没有 name 字段的扫描响应"也能认出来 —— 只按名字认会漏掉那类包，
 * 这正是上一轮误判"手柄不回扫描请求"的原因。 */
static ble_addr_t g_pad_addr;
static int        g_pad_addr_ok;

/* M2b：本机扫描/连接用的地址类型，以及当前连接状态 */
static uint8_t  g_own_addr_type;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int      g_connecting;

static int m2b_try_connect(const ble_addr_t *addr);   /* 定义在下方 M2b 段落 */

static int is_pad_addr(const ble_addr_t *a)
{
    return g_pad_addr_ok && a->type == g_pad_addr.type &&
           memcmp(a->val, g_pad_addr.val, 6) == 0;
}

static void
on_disc(const struct ble_hs_adv_fields *f, const struct ble_gap_disc_desc *disc)
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

    /* M2b：认识手柄了，发起连接（连接后就不再扫描）。 */
    m2b_try_connect(&disc->addr);

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

/* ===================== M2b：连接 + 订阅 0xFFA1 + 帧解析 ===================== */

#define CODEXPAD_DATATYPE_INPUT_STATE  0x01
#define CODEXPAD_UUID_SVC              0xFFA0
#define CODEXPAD_UUID_CHR              0xFFA1
#define CODEXPAD_UUID_CCCD             0x2902

typedef struct __attribute__((packed)) {
    uint32_t buttons;      /* 小端 */
    uint8_t  axes[4];      /* Lx, Ly, Rx, Ry；中心 0x80 */
} codexpad_state_t;

/* --- CRC8：CRC-8/SAE-J1850，poly 0x1D / init 0xFF / xorout 0xFF（附录 A.3）
 *     用位运算而不是查表：这张表在文档 A.6 里是空的，位运算等价且已验证
 *     与标准测试向量一致（tools/frame_test.py 里 crc8("123456789") == 0x4B）。--- */
static uint8_t crc8_calc(const uint8_t *d, size_t n)
{
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x1D) : (uint8_t)(c << 1);
        }
    }
    return c ^ 0xFF;
}

static uint8_t s_buf[64];
static size_t  s_len;
static int     s_esc, s_in_frame;
static uint32_t g_crc_bad;

static void feed_byte(uint8_t b);   /* 带帧格式的兜底解析，定义在下面 */

/* ===================== M2c：摇杆 → ω_ref =====================
 *
 * ⚠ 关键设计："包不来" ≠ "掉线"。
 * 实测这只手柄的 notify 是**变化驱动**：动的时候 ~32 Hz，手一停一包都不发。
 * 所以控制环绝不能把"多久没收到包"当成失效判据 —— 那样摇杆停在中间位置时
 * 系统会误判掉线而急停。
 *
 * 方案（"保持上一次的值"）：
 *   - 一个长度 1 的**覆盖式队列**存"最新状态"。notify 来了就覆盖，
 *     没来就一直是老值 → 控制环永远读到"最后一次已知状态"，不存在"过期"概念。
 *   - **唯一的清零点是 GAP 层的 DISCONNECT 事件** —— 那是真的掉线。
 *     也就是"失效判据挂在连接状态上，不挂在数据流上"。
 */
#define M2C_PERIOD_MS    20          /* 控制环周期 50 Hz（手柄最快 ~32 Hz，够用） */
#define OMEGA_MAX_DPS    60.0f       /* 摇杆推满对应的角速度，先取 60 °/s，实测再调 */
#define AXIS_DEADZONE    0.08f       /* 归一化死区，抵消回中不准引起的漂移 */

static QueueHandle_t g_pad_q;        /* codexpad_state_t × 1，覆盖式，单写单读 */

static void state_center(codexpad_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->axes[0] = st->axes[1] = st->axes[2] = st->axes[3] = 0x80;
}

/* 收到一包就把最新状态覆盖进去。没收到包时队列里还是上一次的值。 */
static void push_state(const codexpad_state_t *st)
{
    if (g_pad_q) xQueueOverwrite(g_pad_q, st);
}

/* 真正的失效处理：只在 GAP 断开时调用一次，把状态压回中位（ω_ref → 0）。 */
static void push_center(void)
{
    codexpad_state_t z;
    state_center(&z);
    push_state(&z);
}

/* 归一化到 [-1, +1]。⚠ 分母要**按方向取**：中心到 0 是 128 个计数、
 * 中心到 255 是 127 个，统一用 127 会让负半轴超到 -1.008（满量程打出 -60.47 而不是 -60.00）。 */
static inline float axis_norm(uint8_t raw)
{
    float v = (float)raw - 128.0f;
    return v / (v < 0.0f ? 128.0f : 127.0f);
}

static inline float apply_deadzone(float v)
{
    return (v > -AXIS_DEADZONE && v < AXIS_DEADZONE) ? 0.0f : v;
}

/* 控制环：永远用"最后一次已知状态"算 ω_ref，只在数值变化时打印。
 * 没有 notify 的时段（手不动）它读到的是同一个值 → 不打印、也不清零。 */
static void m2c_task(void *arg)
{
    codexpad_state_t st;
    codexpad_state_t last;
    state_center(&last);

    while (1) {
        if (xQueuePeek(g_pad_q, &st, 0) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(M2C_PERIOD_MS));
            continue;
        }

        float pan  = apply_deadzone(axis_norm(st.axes[0])) * OMEGA_MAX_DPS;  /* 左摇杆 X */
        float tilt = apply_deadzone(axis_norm(st.axes[3])) * OMEGA_MAX_DPS;  /* 右摇杆 Y */

        if (pan == 0.0f && tilt == 0.0f) {
            pan = 0.0f; tilt = 0.0f;                 /* 消掉 -0.0，免得每次打印都不一样 */
        }

        /* 变化才打印，否则 50 Hz 会把日志刷爆（手柄静置时这里一次都不打） */
        if (st.buttons != last.buttons ||
            st.axes[0] != last.axes[0] || st.axes[1] != last.axes[1] ||
            st.axes[2] != last.axes[2] || st.axes[3] != last.axes[3]) {
            last = st;

            char names[160];
            names[0] = '\0';
            for (size_t i = 0; i < sizeof(k_btn_names) / sizeof(k_btn_names[0]); ++i) {
                if (st.buttons & k_btn_names[i].bit) {
                    strcat(names, " ");
                    strcat(names, k_btn_names[i].name);
                }
            }
            printf("[ω_ref] pan=%+7.2f  tilt=%+7.2f  °/s   "
                   "raw Lx=%3u Ly=%3u Rx=%3u Ry=%3u   buttons=0x%05lX [%s ]\n",
                   pan, tilt, st.axes[0], st.axes[1], st.axes[2], st.axes[3],
                   (unsigned long)st.buttons, names);
        }
        vTaskDelay(pdMS_TO_TICKS(M2C_PERIOD_MS));
    }
}

/* 一个 notify 包。实测这只手柄发的是 8 字节裸状态（没有 0xAA…0x55 封装），
 * 但仍保留带帧格式的兜底解析 —— 万一某些情况下它真发帧。 */
static void handle_notify(const uint8_t *buf, uint16_t len)
{
    static uint32_t ntotal;
    static int64_t  t0;
    static uint16_t lmin = 0xFFFF, lmax;
    if (t0 == 0) t0 = esp_timer_get_time();
    ntotal++;
    if (len < lmin) lmin = len;
    if (len > lmax) lmax = len;

    if (ntotal % 200 == 0) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        printf("[notify 速率] 累计 %lu 包 / %d ms → %.1f Hz（长度 %u~%u，CRC 失败 %lu）\n",
               (unsigned long)ntotal, (int)ms,
               1000.0 * (double)ntotal / (double)ms, lmin, lmax,
               (unsigned long)g_crc_bad);
    }

    /* 头几包打原始 hex：这是"格式到底是什么"的唯一现场证据 */
    if (ntotal <= 8 || len != sizeof(codexpad_state_t)) {
        printf("[notify 原始 #%lu len=%u]:", (unsigned long)ntotal, len);
        for (uint16_t i = 0; i < len; ++i) printf(" %02X", buf[i]);
        printf("\n");
    }

    if (len == sizeof(codexpad_state_t)) {       /* 实测格式：8 字节裸状态 */
        codexpad_state_t st;
        memcpy(&st, buf, sizeof(st));
        push_state(&st);
        return;
    }
    for (uint16_t i = 0; i < len; ++i) feed_byte(buf[i]);   /* 兜底：按帧解析 */
}

static void feed_byte(uint8_t b)
{
    if (!s_in_frame) {
        if (b == 0xAA) { s_in_frame = 1; s_esc = 0; s_len = 0; }
        return;
    }
    if (b == 0x55) {                       /* 帧尾 → 校验 */
        s_in_frame = 0;
        if (s_len < 2) return;
        size_t pl = s_len - 1;
        if (crc8_calc(s_buf, pl) == s_buf[pl]) {
            if (pl >= 1 + sizeof(codexpad_state_t) &&
                s_buf[0] == CODEXPAD_DATATYPE_INPUT_STATE) {
                codexpad_state_t st;
                memcpy(&st, s_buf + 1, sizeof(st));
                push_state(&st);
            }
        } else {
            g_crc_bad++;
        }
        return;
    }
    if (b == 0xDB) { s_esc = 1; return; }  /* 转义态：下一字节 ^0x20 */
    if (s_esc) { b ^= 0x20; s_esc = 0; }

    if (s_len < sizeof(s_buf)) s_buf[s_len++] = b;
    else s_in_frame = 0;                   /* 缓冲溢出，重来 */
}

/* --- GATT 发现链：把每个服务的特征全打出来（含文档没写的 0xFFE0），
 *     顺路记下 0xFFA1 的 val_handle 与它的 CCCD，最后订阅。--- */
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

static int m2b_try_connect(const ble_addr_t *addr)
{
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE || g_connecting) return 0;

    g_connecting = 1;
    /* 发起连接前必须停掉扫描（NimBLE 不允许边扫边连） */
    ble_gap_disc_cancel();

    printf("\n[M2b] 发起连接 → %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr->val[5], addr->val[4], addr->val[3],
           addr->val[2], addr->val[1], addr->val[0]);

    int rc = ble_gap_connect(g_own_addr_type, addr, 30000, NULL,
                             blecent_gap_event, NULL);
    if (rc != 0) {
        printf("[M2b] 连接发起失败 rc=%d，恢复扫描\n", rc);
        g_connecting = 0;
        blecent_scan();
    }
    return 0;
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Application Callback. Called when the custom subscribable chatacteristic
 * in the remote GATT server is read.
 * Expect to get the recently written data.
 **/
static int
blecent_on_custom_read(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr,
                       void *arg)
{
    MODLOG_DFLT(INFO,
                "Read complete for the subscribable characteristic; "
                "status=%d conn_handle=%d", error->status, conn_handle);
    if (error->status == 0) {
        MODLOG_DFLT(INFO, " attr_handle=%d value=", attr->handle);
        print_mbuf(attr->om);
    }
    MODLOG_DFLT(INFO, "\n");

    return 0;
}

/**
 * Application Callback. Called when the custom subscribable characteristic
 * in the remote GATT server is written to.
 * Client has previously subscribed to this characeteristic,
 * so expect a notification from the server.
 **/
static int
blecent_on_custom_write(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr,
                        void *arg)
{
    const struct peer_chr *chr;
    const struct peer *peer;
    int rc;

    MODLOG_DFLT(INFO,
                "Write to the custom subscribable characteristic complete; "
                "status=%d conn_handle=%d attr_handle=%d\n",
                error->status, conn_handle, attr->handle);

    peer = peer_find(conn_handle);
    if (peer == NULL) {
        MODLOG_DFLT(WARN,"Peer not found (conn_handle=%d), likely disconnected\n",conn_handle);
        return 0;
    }
    chr = peer_chr_find_uuid(peer,
                             remote_svc_uuid,
                             remote_chr_uuid);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR,
                    "Error: Peer doesn't have the custom subscribable characteristic\n");
        goto err;
    }

    /*** Performs a read on the characteristic, the result is handled in blecent_on_new_read callback ***/
    rc = ble_gattc_read(conn_handle, chr->chr.val_handle,
                        blecent_on_custom_read, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR,
                    "Error: Failed to read the custom subscribable characteristic; "
                    "rc=%d\n", rc);
        goto err;
    }

    return 0;
err:
    /* Terminate the connection */
    return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Application Callback. Called when the custom subscribable characteristic
 * is subscribed to.
 **/
static int
blecent_on_custom_subscribe(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr,
                            void *arg)
{
    const struct peer_chr *chr;
    uint8_t value;
    int rc;
    const struct peer *peer;

    MODLOG_DFLT(INFO,
                "Subscribe to the custom subscribable characteristic complete; "
                "status=%d conn_handle=%d", error->status, conn_handle);

    if (error->status == 0) {
        MODLOG_DFLT(INFO, " attr_handle=%d value=", attr->handle);
        print_mbuf(attr->om);
    }
    MODLOG_DFLT(INFO, "\n");

    peer = peer_find(conn_handle);
    chr = peer_chr_find_uuid(peer,
                             remote_svc_uuid,
                             remote_chr_uuid);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't have the subscribable characteristic\n");
        goto err;
    }

    /* Write 1 byte to the new characteristic to test if it notifies after subscribing */
    value = 0x19;
    rc = ble_gattc_write_flat(conn_handle, chr->chr.val_handle,
                              &value, sizeof(value), blecent_on_custom_write, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR,
                    "Error: Failed to write to the subscribable characteristic; "
                    "rc=%d\n", rc);
        goto err;
    }

    return 0;
err:
    /* Terminate the connection */
    return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Performs 3 operations on the remote GATT server.
 * 1. Subscribes to a characteristic by writing 0x10 to it's CCCD.
 * 2. Writes to the characteristic and expect a notification from remote.
 * 3. Reads the characteristic and expect to get the recently written information.
 **/
static void
blecent_custom_gatt_operations(const struct peer* peer)
{
    const struct peer_dsc *dsc;
    int rc;
    uint8_t value[2];

    dsc = peer_dsc_find_uuid(peer,
                             remote_svc_uuid,
                             remote_chr_uuid,
                             BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
    if (dsc == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer lacks a CCCD for the subscribable characteristic\n");
        goto err;
    }

    /*** Write 0x00 and 0x01 (The subscription code) to the CCCD ***/
    value[0] = 1;
    value[1] = 0;
    rc = ble_gattc_write_flat(peer->conn_handle, dsc->dsc.handle,
                              value, sizeof(value), blecent_on_custom_subscribe, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR,
                    "Error: Failed to subscribe to the subscribable characteristic; "
                    "rc=%d\n", rc);
        goto err;
    }

    return;
err:
    /* Terminate the connection */
    ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Application callback.  Called when the attempt to subscribe to notifications
 * for the ANS Unread Alert Status characteristic has completed.
 */
static int
blecent_on_subscribe(uint16_t conn_handle,
                     const struct ble_gatt_error *error,
                     struct ble_gatt_attr *attr,
                     void *arg)
{
    struct peer *peer;

    MODLOG_DFLT(INFO, "Subscribe complete; status=%d conn_handle=%d "
                "attr_handle=%d\n",
                error->status, conn_handle, attr->handle);

    peer = peer_find(conn_handle);
    if (peer == NULL) {
        MODLOG_DFLT(ERROR, "Error in finding peer, aborting...");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    /* Subscribe to, write to, and read the custom characteristic*/
    blecent_custom_gatt_operations(peer);

    return 0;
}

/**
 * Application callback.  Called when the write to the ANS Alert Notification
 * Control Point characteristic has completed.
 */
static int
blecent_on_write(uint16_t conn_handle,
                 const struct ble_gatt_error *error,
                 struct ble_gatt_attr *attr,
                 void *arg)
{
    MODLOG_DFLT(INFO,
                "Write complete; status=%d conn_handle=%d attr_handle=%d\n",
                error->status, conn_handle, attr->handle);

    /* Subscribe to notifications for the Unread Alert Status characteristic.
     * A central enables notifications by writing two bytes (1, 0) to the
     * characteristic's client-characteristic-configuration-descriptor (CCCD).
     */
    const struct peer_dsc *dsc;
    uint8_t value[2];
    int rc;
    const struct peer *peer = peer_find(conn_handle);
    if (peer == NULL) {
        MODLOG_DFLT(ERROR, "Error: peer not found for conn_handle=%d", conn_handle);
        return ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);  // Use conn_handle to avoid dereference
    }
    dsc = peer_dsc_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLECENT_SVC_ALERT_UUID),
                             BLE_UUID16_DECLARE(BLECENT_CHR_UNR_ALERT_STAT_UUID),
                             BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
    if (dsc == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer lacks a CCCD for the Unread Alert "
                    "Status characteristic\n");
        goto err;
    }

    value[0] = 1;
    value[1] = 0;
    rc = ble_gattc_write_flat(conn_handle, dsc->dsc.handle,
                              value, sizeof value, blecent_on_subscribe, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to subscribe to characteristic; "
                    "rc=%d\n", rc);
        goto err;
    }

    return 0;
err:
    /* Terminate the connection. */
    return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Application callback.  Called when the read of the ANS Supported New Alert
 * Category characteristic has completed.
 */
static int
blecent_on_read(uint16_t conn_handle,
                const struct ble_gatt_error *error,
                struct ble_gatt_attr *attr,
                void *arg)
{
    MODLOG_DFLT(INFO, "Read complete; status=%d conn_handle=%d", error->status,
                conn_handle);
    if (error->status == 0) {
        MODLOG_DFLT(INFO, " attr_handle=%d value=", attr->handle);
        print_mbuf(attr->om);
    }
    MODLOG_DFLT(INFO, "\n");

    /* Write two bytes (99, 100) to the alert-notification-control-point
     * characteristic.
     */
    const struct peer_chr *chr;
    uint8_t value[2];
    int rc;
    const struct peer *peer = peer_find(conn_handle);
    if (peer == NULL) {
        MODLOG_DFLT(ERROR, "Error: peer not found for conn_handle=%d", conn_handle);
        return ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    chr = peer_chr_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLECENT_SVC_ALERT_UUID),
                             BLE_UUID16_DECLARE(BLECENT_CHR_ALERT_NOT_CTRL_PT));
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't support the Alert "
                    "Notification Control Point characteristic\n");
        goto err;
    }

    value[0] = 99;
    value[1] = 100;
    rc = ble_gattc_write_flat(conn_handle, chr->chr.val_handle,
                              value, sizeof value, blecent_on_write, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to write characteristic; rc=%d\n",
                    rc);
        goto err;
    }

    return 0;
err:
    /* Terminate the connection. */
    return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Performs three GATT operations against the specified peer:
 * 1. Reads the ANS Supported New Alert Category characteristic.
 * 2. After read is completed, writes the ANS Alert Notification Control Point characteristic.
 * 3. After write is completed, subscribes to notifications for the ANS Unread Alert Status
 *    characteristic.
 *
 * If the peer does not support a required service, characteristic, or
 * descriptor, then the peer lied when it claimed support for the alert
 * notification service!  When this happens, or if a GATT procedure fails,
 * this function immediately terminates the connection.
 */
static void
blecent_read_write_subscribe(const struct peer *peer)
{
    const struct peer_chr *chr;
    int rc;

    /* Read the supported-new-alert-category characteristic. */
    chr = peer_chr_find_uuid(peer,
                             BLE_UUID16_DECLARE(BLECENT_SVC_ALERT_UUID),
                             BLE_UUID16_DECLARE(BLECENT_CHR_SUP_NEW_ALERT_CAT_UUID));
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "Error: Peer doesn't support the Supported New "
                    "Alert Category characteristic\n");
        goto err;
    }

    rc = ble_gattc_read(peer->conn_handle, chr->chr.val_handle,
                        blecent_on_read, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to read characteristic; rc=%d\n",
                    rc);
        goto err;
    }

    return;
err:
    /* Terminate the connection. */
    ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * Called when service discovery of the specified peer has completed.
 */
static void
blecent_on_disc_complete(const struct peer *peer, int status, void *arg)
{

    if (status != 0) {
        /* Service discovery failed.  Terminate the connection. */
        MODLOG_DFLT(ERROR, "Error: Service discovery failed; status=%d "
                    "conn_handle=%d\n", status, peer->conn_handle);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* Service discovery has completed successfully.  Now we have a complete
     * list of services, characteristics, and descriptors that the peer
     * supports.
     */
    MODLOG_DFLT(INFO, "Service discovery complete; status=%d "
                "conn_handle=%d\n", status, peer->conn_handle);

    /* Now perform three GATT procedures against the peer: read,
     * write, and subscribe to notifications for the ANS service.
     */
    blecent_read_write_subscribe(peer);
}
#endif  //MYNEWT_VAL(BLE_GATTC)

/**
 * Initiates the GAP general discovery procedure.
 */
static void
blecent_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }
    g_own_addr_type = own_addr_type;   /* M2b 发起连接时要用同一个 */

    /* ⚠ 必须为 0：手柄靠"重复广播"持续上报按键状态，
     * 过滤掉重复就只看得到第一帧，按什么键都不会变了（执行文档 §4.2）。
     */
    disc_params.filter_duplicates = 0;

    /* ⚠ 必须为 0（主动扫描）：只被动扫描的话不会发 SCAN_REQ，
     * 就拿不到 SCAN_RSP —— 而手柄的厂商数据（按键状态）在扫描响应里。
     * blecent 原版这里写的是 1，照抄会把这一步卡死（执行文档 §4.2）。
     */
    disc_params.passive = 0;

    /* Use defaults for the rest of the parameters. */
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      blecent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error initiating GAP discovery procedure; rc=%d\n",
                    rc);
    }
}

/**
 * Indicates whether we should try to connect to the sender of the specified
 * advertisement.  The function returns a positive result if the device
 * advertises connectability and support for the Alert Notification service.
 */
#if CONFIG_EXAMPLE_EXTENDED_ADV
static int
ext_blecent_should_connect(const struct ble_gap_ext_disc_desc *disc)
{
    int offset = 0;
    int ad_struct_len = 0;
#if CONFIG_EXAMPLE_USE_CI_ADDRESS
    uint32_t *addr_offset;
#endif // CONFIG_EXAMPLE_USE_CI_ADDRESS
    uint8_t test_addr[6];
    if (disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
        return 0;
    }
    if (strlen(CONFIG_EXAMPLE_PEER_ADDR) && (strncmp(CONFIG_EXAMPLE_PEER_ADDR, "ADDR_ANY", strlen    ("ADDR_ANY")) != 0)) {
#if !CONFIG_EXAMPLE_USE_CI_ADDRESS
        ESP_LOGI(tag, "Peer address from menuconfig: %s", CONFIG_EXAMPLE_PEER_ADDR);
        /* Convert string to address */
        peer_addr_parse(CONFIG_EXAMPLE_PEER_ADDR, test_addr);
#endif

#if CONFIG_EXAMPLE_USE_CI_ADDRESS
	addr_offset = (uint32_t *)&test_addr[1];
        *addr_offset = atoi(CONFIG_EXAMPLE_PEER_ADDR);
        test_addr[5] = 0xC3;
        test_addr[0] = TEST_CI_ADDRESS_CHIP_OFFSET;
#endif
	if (memcmp(test_addr, disc->addr.val, sizeof(disc->addr.val)) != 0) {
	    return 0;
        }
    }

    /* The device has to advertise support for the Alert Notification
    * service (0x1811).
    */
    while (offset < disc->length_data) {
        ad_struct_len = disc->data[offset];

        if (ad_struct_len == 0 || offset + ad_struct_len + 1 > disc->length_data) {
            break;
        }

        /* Search if ANS UUID (0x1811) is advertised */
        if (ad_struct_len >= 3 && (disc->data[offset + 1] == 0x02 || disc->data[offset + 1] == 0x03)) {
            for (int i = 2; i + 1 <= ad_struct_len; i += 2) {
                if (disc->data[offset + i] == 0x18 && disc->data[offset + i + 1] == 0x11) {
                    return 1;
                }
            }
        }

        offset += ad_struct_len + 1;
    }

    return 0;
}
#else
static int
blecent_should_connect(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    int rc;
    int i;
#if CONFIG_EXAMPLE_USE_CI_ADDRESS
    uint32_t *addr_offset;
#endif // CONFIG_EXAMPLE_USE_CI_ADDRESS
    uint8_t test_addr[6];
    /* The device has to be advertising connectability. */
    if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {

        return 0;
    }

    rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0) {
        return 0;
    }

    if (strlen(CONFIG_EXAMPLE_PEER_ADDR) && (strncmp(CONFIG_EXAMPLE_PEER_ADDR, "ADDR_ANY", strlen("ADDR_ANY")) != 0)) {
        ESP_LOGI(tag, "Peer address from menuconfig: %s", CONFIG_EXAMPLE_PEER_ADDR);
#if !CONFIG_EXAMPLE_USE_CI_ADDRESS
        /* Convert string to address */
        peer_addr_parse(CONFIG_EXAMPLE_PEER_ADDR, test_addr);
        printf("peer-->  %s\n", addr_str(test_addr));
#endif
#if CONFIG_EXAMPLE_USE_CI_ADDRESS
	addr_offset = (uint32_t *)&test_addr[1];
        *addr_offset = atoi(CONFIG_EXAMPLE_PEER_ADDR);
        test_addr[5] = 0xC3;
        test_addr[0] = TEST_CI_ADDRESS_CHIP_OFFSET;
#endif

	if (memcmp(test_addr, disc->addr.val, sizeof(disc->addr.val)) != 0) {
            return 0;
        }
    }

    /* The device has to advertise support for the Alert Notification
     * service (0x1811).
     */
    for (i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == BLECENT_SVC_ALERT_UUID) {
            return 1;
        }
    }

    return 0;
}
#endif

/**
 * Connects to the sender of the specified advertisement of it looks
 * interesting.  A device is "interesting" if it advertises connectability and
 * support for the Alert Notification service.
 */
static void
blecent_connect_if_interesting(void *disc)
{
    uint8_t own_addr_type;
    int rc;
    ble_addr_t *addr;

    /* Don't do anything if we don't care about this advertiser. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    if (!ext_blecent_should_connect((struct ble_gap_ext_disc_desc *)disc)) {
        return;
    }
#else
    if (!blecent_should_connect((struct ble_gap_disc_desc *)disc)) {
        return;
    }
#endif

#if !(MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN))
    /* Scanning must be stopped before a connection can be initiated. */
    rc = ble_gap_disc_cancel();
    if (rc != 0) {
        MODLOG_DFLT(DEBUG, "Failed to cancel scan; rc=%d\n", rc);
        return;
    }
#endif

    /* Figure out address to use for connect (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Try to connect the the advertiser.  Allow 30 seconds (30000 ms) for
     * timeout.
     */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    addr = &((struct ble_gap_ext_disc_desc *)disc)->addr;
#else
    addr = &((struct ble_gap_disc_desc *)disc)->addr;
#endif

    rc = ble_gap_connect(own_addr_type, addr, 30000, NULL,
                         blecent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to connect to device; addr_type=%d "
                    "addr=%s; rc=%d\n",
                    addr->type, addr_str(addr->val), rc);
        return;
    }
}

#if MYNEWT_VAL(BLE_POWER_CONTROL)
static void blecent_power_control(uint16_t conn_handle)
{
    int rc;

    rc = ble_gap_read_remote_transmit_power_level(conn_handle, 0x01 );  // Attempting on LE 1M phy
    assert (rc == 0);

    rc = ble_gap_set_transmit_power_reporting_enable(conn_handle, 0x01, 0x01);
    assert (rc == 0);

    rc = ble_gap_set_path_loss_reporting_param(conn_handle, 60, 10, 30, 10, 2 ); //demo values
    assert (rc == 0);

    rc = ble_gap_set_path_loss_reporting_enable(conn_handle, 0x01);
    assert (rc == 0);
}
#endif

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that is
 * established.  blecent uses the same callback for all connections.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  blecent.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
blecent_gap_event(struct ble_gap_event *event, void *arg)
{
#if NIMBLE_BLE_CONNECT
    struct ble_gap_conn_desc desc;
#endif
    struct ble_hs_adv_fields fields;
#if MYNEWT_VAL(BLE_HCI_VS)
#if MYNEWT_VAL(BLE_POWER_CONTROL)
    struct ble_gap_set_auto_pcl_params params;
#endif
#endif
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);
        if (rc != 0) {
            return 0;
        }

        /* An advertisement report was received during GAP discovery. */
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

        /* M2a：认出 CodexPad 就解析广播里的按键位。这一步故意不发起连接。 */
        on_disc(&fields, &event->disc);
        return 0;
#if NIMBLE_BLE_CONNECT
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        if (event->connect.status == 0) {
            /* Connection successfully established. */
            MODLOG_DFLT(INFO, "Connection established ");

            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            print_conn_desc(&desc);
            MODLOG_DFLT(INFO, "\n");

            /* Remember peer. */
            rc = peer_add(event->connect.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Failed to add peer; rc=%d\n", rc);
                return 0;
            }

#if MYNEWT_VAL(BLE_POWER_CONTROL)
            blecent_power_control(event->connect.conn_handle);
#endif

#if MYNEWT_VAL(BLE_HCI_VS)
#if MYNEWT_VAL(BLE_POWER_CONTROL)
	    memset(&params, 0x0, sizeof(struct ble_gap_set_auto_pcl_params));
	    params.conn_handle = event->connect.conn_handle;
            rc = ble_gap_set_auto_pcl_param(&params);
            if (rc != 0) {
                MODLOG_DFLT(INFO, "Failed to send VSC  %x \n", rc);
                return 0;
            }
            else {
               MODLOG_DFLT(INFO, "Successfully issued VSC , rc = %d \n", rc);
	    }
#endif
#endif

#if CONFIG_EXAMPLE_ENCRYPTION
            /** Initiate security - It will perform
             * Pairing (Exchange keys)
             * Bonding (Store keys)
             * Encryption (Enable encryption)
             * Will invoke event BLE_GAP_EVENT_ENC_CHANGE
             **/
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(INFO, "Security could not be initiated, rc = %d\n", rc);
                return ble_gap_terminate(event->connect.conn_handle,
                                         BLE_ERR_REM_USER_CONN_TERM);
            } else {
                MODLOG_DFLT(INFO, "Connection secured\n");
            }
#else
#if MYNEWT_VAL(BLE_GATTC)
#if MYNEWT_VAL(BLE_GATT_CACHING_ASSOC_ENABLE)
            rc =  ble_gattc_cache_assoc(desc.peer_id_addr);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Cache Association Failed; rc=%d\n", rc);
                return 0;
            }
#else
            /* M2b：不走 blecent 那套通用发现，直接走自己的链 ——
             * 服务 0xFFA0 → 特征 0xFFA1 → CCCD 0x2902 → 订阅 notify */
            g_conn_handle = event->connect.conn_handle;
            g_connecting  = 0;
            g_svc_n = g_svc_i = g_chr_val_handle = g_cccd_handle = 0;
            rc = ble_gattc_disc_all_svcs(g_conn_handle, on_svc_disc, NULL);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "发现服务失败; rc=%d\n", rc);
                return 0;
            }
#endif // BLE_GATT_CACHING_ASSOC_ENABLE
#endif // BLE_GATTC
#endif // EXAMPLE_ENCRYPTION
        } else {
            /* Connection attempt failed; resume scanning. */
            MODLOG_DFLT(ERROR, "Error: Connection failed; status=%d\n",
                        event->connect.status);
            blecent_scan();
        }

        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Connection terminated. */
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");

        /* Forget about peer. */
        peer_delete(event->disconnect.conn.conn_handle);

        /* M2b：清掉连接状态，下面的 blecent_scan() 会重新扫到并自动重连（AC-BLE-5） */
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_connecting  = 0;
        g_svc_n = g_svc_i = g_chr_val_handle = g_cccd_handle = 0;

        /* M2c：**唯一**的清零点。真掉线 → 手柄状态压回中位，ω_ref 立刻归零（AC-BLE-8）。
         * 注意这里不是"超时清零"：手柄静置不发包是正常的，不受影响。 */
        push_center();

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
        /* Reset EATT config */
        bearers = 0;
        for (int i = 0; i < MYNEWT_VAL(BLE_EATT_CHAN_NUM); i++) {
            cids[i] = 0;
        }
#endif

        /* Resume scanning. */
        blecent_scan();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        MODLOG_DFLT(INFO, "discovery complete; reason=%d\n",
                    event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        MODLOG_DFLT(INFO, "encryption change event; status=%d ",
                    event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        print_conn_desc(&desc);
#if !MYNEWT_VAL(BLE_EATT_CHAN_NUM)
#if CONFIG_EXAMPLE_ENCRYPTION && MYNEWT_VAL(BLE_GATTC)
#if MYNEWT_VAL(BLE_GATT_CACHING_ASSOC_ENABLE)
        rc =  ble_gattc_cache_assoc(desc.peer_id_addr);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Cache Association Failed; rc=%d\n", rc);
            return 0;
        }
#else
        /*** Go for service discovery after encryption has been successfully enabled ***/
        rc = peer_disc_all(event->enc_change.conn_handle,
                           blecent_on_disc_complete, NULL);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
            return 0;
        }
#endif // BLE_GATT_CACHING_ASSOC_ENABLE
#endif // EXAMPLE_ENCRYPTION
#endif
        return 0;

    case BLE_GAP_EVENT_CACHE_ASSOC:
#if MYNEWT_VAL(BLE_GATT_CACHING_ASSOC_ENABLE)
          /* Cache association result for this connection */
          MODLOG_DFLT(INFO, "cache association; conn_handle=%d status=%d cache_state=%s\n",
                      event->cache_assoc.conn_handle,
                      event->cache_assoc.status,
                      (event->cache_assoc.cache_state == 0) ? "INVALID" : "LOADED");
          /* Perform service discovery */
          rc = peer_disc_all(event->cache_assoc.conn_handle,
                             blecent_on_disc_complete, NULL);
          if(rc != 0) {
                MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
                return 0;
          }
#endif
          return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t  buf[64];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        handle_notify(buf, len);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

#if CONFIG_EXAMPLE_EXTENDED_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        /* An advertisement report was received during GAP discovery. */
        ext_print_adv_report(&event->ext_disc);

        blecent_connect_if_interesting(&event->ext_disc);
        return 0;
#endif

#if MYNEWT_VAL(BLE_POWER_CONTROL)
    case BLE_GAP_EVENT_TRANSMIT_POWER:
	MODLOG_DFLT(INFO, "Transmit power event : status=%d conn_handle=%d reason=%d "
                          "phy=%d power_level=%d power_level_flag=%d delta=%d",
		    event->transmit_power.status,
		    event->transmit_power.conn_handle,
		    event->transmit_power.reason,
		    event->transmit_power.phy,
		    event->transmit_power.transmit_power_level,
		    event->transmit_power.transmit_power_level_flag,
		    event->transmit_power.delta);
	return 0;

    case BLE_GAP_EVENT_PATHLOSS_THRESHOLD:
	MODLOG_DFLT(INFO, "Pathloss threshold event : conn_handle=%d current path loss=%d "
                          "zone_entered =%d",
		    event->pathloss_threshold.conn_handle,
		    event->pathloss_threshold.current_path_loss,
		    event->pathloss_threshold.zone_entered);
	return 0;
#endif

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
    case BLE_GAP_EVENT_EATT:
    int i;
    MODLOG_DFLT(INFO, "EATT %s : conn_handle=%d cid=%d",
            event->eatt.status ? "disconnected" : "connected",
            event->eatt.conn_handle,
            event->eatt.cid);
    if (event->eatt.status) {
        /* Remove CID from the list of saved CIDs */
        for (i = 0; i < bearers; i++) {
            if (cids[i] == event->eatt.cid) {
                break;
            }
        }
        while (i < (bearers - 1)) {
            cids[i] = cids[i + 1];
            i += 1;
        }
        cids[i] = 0;

        /* Now Abort */
        return 0;
    }
    cids[bearers] = event->eatt.cid;
    bearers += 1;
    if (bearers != MYNEWT_VAL(BLE_EATT_CHAN_NUM)) {
        /* Wait until all EATT bearers are connected before proceeding */
        return 0;
    }
    /* Set the default bearer to use for further procedures */
    rc = ble_att_set_default_bearer_using_cid(event->eatt.conn_handle, cids[0]);
    if (rc != 0) {
        MODLOG_DFLT(INFO, "Cannot set default EATT bearer, rc = %d\n", rc);
        return rc;
    }
#if MYNEWT_VAL(BLE_GATTC)
    /* Perform service discovery */
    rc = peer_disc_all(event->eatt.conn_handle,
                blecent_on_disc_complete, NULL);
    if(rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
        return 0;
    }
#endif
#endif
        return 0;

#endif
    default:
        return 0;
    }
}

static void
blecent_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
blecent_on_sync(void)
{
    int rc;

    /* Make sure we have proper identity address set (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);


#if !CONFIG_EXAMPLE_INIT_DEINIT_LOOP
    /* Begin scanning for a peripheral to connect to. */
    blecent_scan();
#endif
}

void blecent_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

#if CONFIG_EXAMPLE_INIT_DEINIT_LOOP
/* This function showcases stack init and deinit procedure. */
static void stack_init_deinit(void)
{
    int rc;
    while(1) {

        vTaskDelay(1000);

        ESP_LOGI(tag, "Deinit host");

        rc = nimble_port_stop();
        if (rc == 0) {
            nimble_port_deinit();
        } else {
            ESP_LOGI(tag, "Nimble port stop failed, rc = %d", rc);
            break;
        }

        vTaskDelay(1000);

        ESP_LOGI(tag, "Init host");

        rc = nimble_port_init();
        if (rc != ESP_OK) {
            ESP_LOGI(tag, "Failed to init nimble %d ", rc);
            break;
        }

        nimble_port_freertos_init(blecent_host_task);

        ESP_LOGI(tag, "Waiting for 1 second");
    }
}
#endif

void
app_main(void)
{
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if  (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to init nimble %d ", ret);
        return;
    }

    /* M2c：覆盖式队列（长度 1）初值 = 中位，控制环一开始就有值可读，
     * 不用等第一个 notify。之后每次 notify 覆盖它，掉线时由 push_center() 复位。 */
    g_pad_q = xQueueCreate(1, sizeof(codexpad_state_t));
    assert(g_pad_q != NULL);
    push_center();
    xTaskCreate(m2c_task, "m2c", 4096, NULL, 5, NULL);

    /* Configure the host. */
    ble_hs_cfg.reset_cb = blecent_on_reset;
    ble_hs_cfg.sync_cb = blecent_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if NIMBLE_BLE_CONNECT
#if MYNEWT_VAL(STATIC_PASSKEY)
    /* WARNING: Hardcoded passkey for demonstration only.
     * In production, generate a random passkey per pairing. */
    ble_sm_configure_static_passkey(456789, true);
#endif

    int rc;
    /* Initialize data structures to track connected peers. */
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 64, 64, 64, 64);
    assert(rc == 0);
#else
    rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 64, 64, 64);
    assert(rc == 0);
#endif
#endif

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    int m;
    /* Set the default device name. */
    m = ble_svc_gap_device_name_set("nimble-blecent");
    assert(m == 0);
#endif

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(blecent_host_task);

#if CONFIG_EXAMPLE_INIT_DEINIT_LOOP
    stack_init_deinit();
#endif

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
    bearers = 0;
    for (int i = 0; i < MYNEWT_VAL(BLE_EATT_CHAN_NUM); i++) {
        cids[i] = 0;
    }
#endif

}
