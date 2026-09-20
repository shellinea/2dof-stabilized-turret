/*
 * SPDX-License-Identifier: Apache-2.0
 * M2b/M2c：notify 载荷 → 覆盖队列 → 摇杆映射成 ω_ref。
 *
 * ⚠ 关键设计：「包不来」≠「掉线」。
 * 实测这只手柄的 notify 是**变化驱动**：动的时候 ~32 Hz，手一停一包都不发。
 * 所以控制环绝不能把「多久没收到包」当成失效判据 —— 那样摇杆停在中间位置时
 * 系统会误判掉线而急停。
 *
 * 方案（「保持上一次的值」）：
 *   - 一个长度 1 的**覆盖式队列**存「最新状态」。notify 来了就覆盖，
 *     没来就一直是老值 → 控制环永远读到「最后一次已知状态」，不存在「过期」概念。
 *   - **唯一的清零点是 GAP 层的 DISCONNECT 事件** —— 那是真的掉线。
 *     也就是「失效判据挂在连接状态上，不挂在数据流上」。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "codexpad.h"

/* 17 键位表（附录 A.4）*/
const codexpad_btn_t k_btn_names[] = {
    { 1u << 0,  "Up"    }, { 1u << 1,  "Down"  }, { 1u << 2,  "Left"  },
    { 1u << 3,  "Right" }, { 1u << 4,  "Sq/X"  }, { 1u << 5,  "Tr/Y"  },
    { 1u << 6,  "X/A"   }, { 1u << 7,  "Cir/B" }, { 1u << 8,  "L1"    },
    { 1u << 9,  "L2"    }, { 1u << 10, "L3"    }, { 1u << 11, "R1"    },
    { 1u << 12, "R2"    }, { 1u << 13, "R3"    }, { 1u << 14, "Select"},
    { 1u << 15, "Start" }, { 1u << 16, "Home"  },
};
const size_t k_btn_names_n = sizeof(k_btn_names) / sizeof(k_btn_names[0]);

#define M2C_PERIOD_MS    20          /* 控制环周期 50 Hz（手柄最快 ~32 Hz，够用）*/
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
void pad_input_center(void)
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

/* 控制环：永远用「最后一次已知状态」算 ω_ref，只在数值变化时打印。
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

        /* 变化才打印，否则 50 Hz 会把日志刷爆（手柄静置时这里一次都不打）*/
        if (st.buttons != last.buttons ||
            st.axes[0] != last.axes[0] || st.axes[1] != last.axes[1] ||
            st.axes[2] != last.axes[2] || st.axes[3] != last.axes[3]) {
            last = st;

            char names[160];
            names[0] = '\0';
            for (size_t i = 0; i < k_btn_names_n; ++i) {
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

/* 一个 notify 包。实测这只手柄发的是 8 字节裸状态：没有 0xAA…0x55 帧头、
 * 没有转义、没有 CRC。长度不等于 8 的直接丢弃（已在上面打过原始 hex）。 */
void pad_input_on_notify(const uint8_t *buf, uint16_t len)
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
        printf("[notify 速率] 累计 %lu 包 / %d ms → %.1f Hz（长度 %u~%u）\n",
               (unsigned long)ntotal, (int)ms,
               1000.0 * (double)ntotal / (double)ms, lmin, lmax);
    }

    /* 头几包打原始 hex：这是「格式到底是什么」的唯一现场证据 */
    if (ntotal <= 8 || len != sizeof(codexpad_state_t)) {
        printf("[notify 原始 #%lu len=%u]:", (unsigned long)ntotal, len);
        for (uint16_t i = 0; i < len; ++i) printf(" %02X", buf[i]);
        printf("\n");
    }

    if (len != sizeof(codexpad_state_t)) {
        return;
    }

    codexpad_state_t st;
    memcpy(&st, buf, sizeof(st));
    push_state(&st);
}

/* 建覆盖队列 + 起控制环任务。放在 app_main 里协议栈初始化之后调用。 */
void pad_input_init(void)
{
    g_pad_q = xQueueCreate(1, sizeof(codexpad_state_t));
    assert(g_pad_q != NULL);

    /* 初值 = 中位：第一个 notify 到达前控制环就有值可读 */
    pad_input_center();

    xTaskCreate(m2c_task, "pad_m2c", 4096, NULL, 5, NULL);
}
