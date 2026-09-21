/*
 * SPDX-License-Identifier: Apache-2.0
 * 见 motor_twai.h 的坐标系与线程契约说明。这里只放实现。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

#include "motor_twai.h"

/* ---- 硬件接线（ESP32 上板相位 2/3 实测确认，见 docs/04-ESP32-CAN电机驱动.md）----
 * 收发器是直连，**不像 UART 那样交叉**：T→GPIO4、R→GPIO5、H/L→电机总线。
 * ⚠ 千万别接到 GPIO43/44 —— 那两根是 console UART，压住就没法烧录。 */
#define PIN_TX   4
#define PIN_RX   5
#define BITRATE  500000        /* 与电机总线一致 */

/* ---- Emm 协议 ----
 * 地址**只在扩展帧 ID 里**（ID = addr<<8 | 包号），数据里不放地址；
 * 数据 = 功能码 + 参数 + 校验 0x6B。 */
#define MOTOR_ADDR     1        /* Emm 出厂默认地址 1 */
#define MOTOR_ADDR_2   2        /* 第二台，实测地址就是 2 */
#define MIRROR_ADDR_2  1        /* 非 0 = 2 号镜像安装，世界转角要取反 */

#define OP_READ_POS  0x36       /* 5.5.13 读实时位置 */
#define OP_ENABLE    0xF3       /* F3 AB <state> <snF> 使能/失能 */
#define OP_VELOCITY  0xF6       /* 5.3.7 Emm 速度模式，单位纯 RPM（0-3000） */
#define OP_SYNC      0xFF       /* FF 66 多机同步广播，ID=0x0000 */
#define SYNC_AUX     0x66
#define EMM_CKSUM    0x6B

/* 一轮收发的上限。100 ms 是上板实测够用的值（正常是微秒级）。 */
#define TX_TIMEOUT_MS   100
#define TX_DONE_MS      200
#define RX_TIMEOUT_US   100000

/* ---- 模块私有状态 ---- */
static twai_node_handle_t s_node;

/* ISR → 任务的单帧中转。功能码/地址双过滤：总线上两台同时在回话，
 * 光凭功能码会张冠李戴（1 号的 3A 被当成 2 号的）。 */
static uint8_t             s_rx_data[TWAI_FRAME_MAX_LEN];
static twai_frame_header_t s_rx_hdr;
static volatile bool       s_rx_got;
static volatile uint8_t    s_want_func;   /* 0 = 不限 */
static volatile uint8_t    s_want_addr;   /* 0 = 不限（0 是广播地址，天然通配） */
static volatile uint32_t   s_err_flags;

/* 在 ISR 里跑：不打印、不阻塞，只拷数据 + 置标志。不合要求的帧就地丢掉
 * （帧已从 FIFO 取走），于是调用方可以安心"一直等到想要的那一帧"。 */
static bool on_rx_done(twai_node_handle_t h, const twai_rx_done_event_data_t *edata,
                       void *ctx)
{
    twai_frame_t f = { 0 };
    f.buffer     = s_rx_data;
    f.buffer_len = sizeof(s_rx_data);

    if (twai_node_receive_from_isr(h, &f) == ESP_OK) {
        if ((s_want_func == 0 ||
             (f.header.dlc >= 1 && s_rx_data[0] == s_want_func)) &&
            (s_want_addr == 0 || (f.header.id >> 8) == s_want_addr)) {
            s_rx_hdr = f.header;
            s_rx_got = true;    /* 先拷完数据再置标志 */
        }
    }
    return false;               /* 不唤醒更高优先级任务 */
}

static bool on_error(twai_node_handle_t h, const twai_error_event_data_t *edata,
                     void *ctx)
{
    s_err_flags |= edata->err_flags.val;    /* ISR 里只做 OR，解码留给任务 */
    return false;
}

bool motor_ready(void)
{
    return s_node != NULL;
}

bool motor_init(void)
{
    if (s_node) {
        return true;
    }

    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx                = PIN_TX,
            .rx                = PIN_RX,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing     = { .bitrate = BITRATE },
        .tx_queue_depth = 4,
        .fail_retry_cnt = 0,
        .flags = {
            /* 总线上有真电机：ACK 必须由电机给。开着 self_test 等于
             * "自己给自己盖章"，电机没接也能过。loopback 同理只属于单节点自检。 */
            .enable_loopback  = 0,
            .enable_self_test = 0,
        },
    };

    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        printf("[motor] twai_new_node_onchip 失败: %s\n", esp_err_to_name(err));
        s_node = NULL;
        return false;
    }

    twai_event_callbacks_t cbs = { .on_rx_done = on_rx_done, .on_error = on_error };
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) {
        printf("[motor] 注册回调失败: %s\n", esp_err_to_name(err));
        twai_node_delete(s_node);
        s_node = NULL;
        return false;
    }

    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        printf("[motor] 使能节点失败: %s\n", esp_err_to_name(err));
        twai_node_delete(s_node);
        s_node = NULL;
        return false;
    }

    s_want_func = 0;
    s_want_addr = 0;
    s_rx_got    = false;
    s_err_flags = 0;
    printf("[motor] TWAI 就绪：tx=GPIO%d rx=GPIO%d %u kbps\n",
           PIN_TX, PIN_RX, (unsigned)(BITRATE / 1000));
    return true;
}

void motor_deinit(void)
{
    if (!s_node) {
        return;
    }
    s_want_addr = 0;                /* 别把过滤器留给下一次 */
    twai_node_disable(s_node);
    twai_node_delete(s_node);
    s_node = NULL;
}

/* ---- 发帧 ---- */

static bool send_raw_frame(uint32_t id, const uint8_t *data, uint16_t len)
{
    twai_frame_t t = { 0 };
    t.header.id  = id;
    t.header.dlc = len;
    t.header.ide = 1;                   /* Emm 一律扩展帧，广播也是 */
    t.buffer     = (uint8_t *)data;
    t.buffer_len = len;

    esp_err_t e = twai_node_transmit(s_node, &t, TX_TIMEOUT_MS);
    if (e == ESP_OK) {
        e = twai_node_transmit_wait_all_done(s_node, TX_DONE_MS);
    }
    if (e != ESP_OK) {
        printf("[motor] 发 ID=0x%03X 失败: %s\n", (unsigned)id, esp_err_to_name(e));
        return false;
    }
    return true;
}

/* 逻辑命令拆帧（tools/zdt_can.py:107 build_frames 的 C 版）：
 *   ≤8 字节 → 一帧，ID=(addr<<8)|0；
 *   >8 字节 → 第 0 帧装前 8 字节，之后每包 7 字节、**开头重复一次功能码**，包号递增。 */
static bool send_payload_addr(uint8_t addr, const uint8_t *payload, uint16_t len)
{
    uint8_t data[TWAI_FRAME_MAX_LEN + 8];
    memcpy(data, payload, len);
    data[len] = EMM_CKSUM;
    const uint16_t n = len + 1;

    uint8_t  buf[TWAI_FRAME_MAX_LEN];
    uint16_t off = 0;
    uint8_t  pkt = 0;
    bool     ok  = true;

    for (;;) {
        uint16_t k = 0;
        if (pkt > 0) {
            buf[k++] = data[0];             /* 后续包开头重复功能码 */
        }
        while (off < n && k < TWAI_FRAME_MAX_LEN) {
            buf[k++] = data[off++];
        }
        if (!send_raw_frame(((uint32_t)addr << 8) | pkt, buf, k)) {
            ok = false;
        }
        if (off >= n) {
            break;
        }
        pkt++;
        vTaskDelay(pdMS_TO_TICKS(3));       /* 分包留间隔防粘包，zdt_can.py:156 */
    }
    return ok;
}

/* ---- 收发一次：发请求 + 等滤波后的那一帧 ---- */

bool motor_read_pos(uint8_t addr, int32_t *mdeg)
{
    /* ⚠ 请求帧**自带校验**，必须发原始帧。不能再走 send_payload_addr ——
     * 那个函数会自己补一个 0x6B，补两次变成 `36 6B 6B`，电机直接不认
     * （第一版就是这么错的：TWAI 建好了、读不回任何东西）。 */
    static const uint8_t req[2] = { OP_READ_POS, EMM_CKSUM };

    s_want_func = OP_READ_POS;
    s_want_addr = addr;
    s_rx_got    = false;
    memset(&s_rx_hdr, 0, sizeof(s_rx_hdr));
    memset(s_rx_data, 0, sizeof(s_rx_data));

    if (!send_raw_frame(((uint32_t)addr << 8) | 0, req, sizeof(req))) {
        s_want_addr = 0;
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    while (!s_rx_got && (esp_timer_get_time() - t0) < RX_TIMEOUT_US) {
        vTaskDelay(1);
    }
    s_want_addr = 0;                /* 过滤器只对当次有效，别漏给下一次 */

    /* 5.5.13 应答：36 + 符号(00=正/01=负) + 位置 4B 大端 + 校验 = 7 字节。
     * Emm 固件位置 0..65535 表示一圈，角度 = 位置 * 360 / 65536。 */
    if (s_rx_hdr.dlc != 7 || s_rx_data[0] != OP_READ_POS ||
        s_rx_data[6] != EMM_CKSUM) {
        return false;
    }

    const uint32_t raw = ((uint32_t)s_rx_data[2] << 24) | ((uint32_t)s_rx_data[3] << 16) |
                         ((uint32_t)s_rx_data[4] <<  8) |  (uint32_t)s_rx_data[5];
    int32_t md = (int32_t)(((uint64_t)raw * 360000ULL) / 65536ULL);
    if (s_rx_data[1] == 0x01) {
        md = -md;
    }
    *mdeg = md;
    return true;
}

bool motor_read_pose(int32_t *tilt_mdeg, int32_t *pan_mdeg)
{
    int32_t p1 = 0, p2 = 0;
    if (!motor_read_pos(MOTOR_ADDR,   &p1)) return false;
    if (!motor_read_pos(MOTOR_ADDR_2, &p2)) return false;

    /* 2 号镜像 ⇒ 世界转角 = −编码器角（tools/gimbal.py:139-141） */
    const int32_t w1 = p1;
    const int32_t w2 = MIRROR_ADDR_2 ? -p2 : p2;
    *tilt_mdeg = (w1 + w2) / 2;
    *pan_mdeg  = (w1 - w2) / 2;
    return true;
}

/* ---- 使能 / 速度 ---- */

bool motor_enable(bool on)
{
    const uint8_t pl[4] = { OP_ENABLE, 0xAB, (uint8_t)(on ? 0x01 : 0x00), 0x00 };
    if (!send_payload_addr(MOTOR_ADDR, pl, sizeof(pl))) return false;
    vTaskDelay(pdMS_TO_TICKS(3));
    return send_payload_addr(MOTOR_ADDR_2, pl, sizeof(pl));
}

/* 单台速度指令：F6 <dir> <vel 2B 大端> <acc> <snF>（5.3.7，Emm）。
 * ⚠ 单位是**纯 RPM**（0-3000）—— X 固件 §5.3.6 那版是 0.1 RPM 标度，别混。
 * snF=0 立即执行：速度是连续的，FF 66 广播只对"缓存多台定位、同一刻触发"有意义。 */
static bool set_velocity(uint8_t addr, int rpm, uint8_t acc)
{
    const uint16_t mag = (uint16_t)((rpm < 0) ? -rpm : rpm);
    const uint8_t  pl[6] = {
        OP_VELOCITY,
        (uint8_t)((rpm < 0) ? 0x01 : 0x00),
        (uint8_t)(mag >> 8),
        (uint8_t)(mag & 0xFF),
        acc,
        0x00,
    };
    return send_payload_addr(addr, pl, sizeof(pl));
}

/* 千分之一度/秒 → RPM（转速 = 角速度 × 60/360 = mdps/6000）。四舍五入。 */
static int rpm_of_mdps(int32_t mdps)
{
    return (int)((mdps >= 0) ? (mdps + 3000) / 6000 : (mdps - 3000) / 6000);
}

bool motor_diff_drive(int32_t wt_mdps, int32_t wp_mdps, uint8_t acc)
{
    if (!s_node) return false;

    const int cmd1 = rpm_of_mdps(wt_mdps + wp_mdps);
    const int cmd2 = rpm_of_mdps(wp_mdps - wt_mdps);   /* 2 号镜像 ⇒ 取反 ω_w2 */
    if (!set_velocity(MOTOR_ADDR, cmd1, acc)) return false;
    vTaskDelay(pdMS_TO_TICKS(3));
    return set_velocity(MOTOR_ADDR_2, cmd2, acc);
}

bool motor_stop(void)
{
    if (!s_node) return false;

    const bool a = set_velocity(MOTOR_ADDR,   0, 0);
    vTaskDelay(pdMS_TO_TICKS(3));
    const bool b = set_velocity(MOTOR_ADDR_2, 0, 0);
    return a && b;
}
