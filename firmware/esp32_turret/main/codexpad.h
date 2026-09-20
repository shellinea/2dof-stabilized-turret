/*
 * SPDX-License-Identifier: Apache-2.0
 * CodexPad-S10 手柄驱动 —— 模块间公共接口
 *
 * 厂商协议均为实测所得，规格与证据见 docs/03-BLE手柄接入执行文档.md。
 */
#ifndef CODEXPAD_H_
#define CODEXPAD_H_

#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"        /* ble_addr_t / ble_gap_disc_desc / ble_gap_event */

/* ---------- 厂商自定义 GATT（不是标准 HID）---------- */
#define CODEXPAD_UUID_SVC   0xFFA0   /* 输入服务 */
#define CODEXPAD_UUID_CHR   0xFFA1   /* 输入特征 */
#define CODEXPAD_UUID_CCCD  0x2902   /* 订阅开关，标准 CCCD */

/* ---------- notify 载荷：裸 8 字节，无帧头 / 无转义 / 无 CRC ---------- */
typedef struct __attribute__((packed)) {
    uint32_t buttons;      /* 小端；位定义见 k_btn_names */
    uint8_t  axes[4];      /* Lx, Ly, Rx, Ry；中心 0x80 */
} codexpad_state_t;

/* ---------- 17 键位表 ---------- */
typedef struct { uint32_t bit; const char *name; } codexpad_btn_t;
extern const codexpad_btn_t k_btn_names[];
extern const size_t         k_btn_names_n;

/* ---------- pad_scan.c：M2a 广播扫描与解析 ---------- */
void pad_scan_on_disc(const struct ble_hs_adv_fields *f,
                      const struct ble_gap_disc_desc *disc);

/* ---------- pad_link.c：M2b GAP —— 扫描 / 连接 / 断线重扫 ---------- */
void pad_link_scan(void);
int  pad_link_connect(const ble_addr_t *addr);

/* ---------- pad_gatt.c：M2b GATT —— 发现服务链 + 订阅 notify ---------- */
void pad_gatt_start(uint16_t conn_handle);
void pad_gatt_reset(void);

/* ---------- pad_input.c：M2b/M2c —— 载荷 → 覆盖队列 → ω_ref ---------- */
void pad_input_init(void);
void pad_input_on_notify(const uint8_t *buf, uint16_t len);
void pad_input_center(void);

#endif /* CODEXPAD_H_ */
