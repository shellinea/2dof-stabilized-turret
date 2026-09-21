/*
 * SPDX-License-Identifier: Apache-2.0
 * 电机 TWAI 驱动 + 差速逆运动学 —— M2e 的"最后一环"。
 *
 * 从 twai_loopback 自检固件里抽出来的**已验证**部分：
 *   Emm 帧格式/拆包、36 读位置、F6 速度模式、cmd1/ cmd2 差速逆解。
 * 抽之前它们在 twai_loopback 自检固件里跑过相位 6（位置模式）+ 相位 7
 * （速度模式），6/0 与 6/0 全通，实测记录见 docs/03-BLE手柄接入执行文档.md §7。
 *
 * 坐标系约定（世界 vs 命令，别混）：
 *   * 世界角速度 ω_ref = (ω_tilt, ω_pan)，俯仰 = (θ1+θ2)/2、自转 = (θ1−θ2)/2
 *   * 命令空间（2 号镜像安装）：cmd1 = ω_tilt + ω_pan，cmd2 = ω_pan − ω_tilt
 *     自检：纯俯仰 ⇒ m1:+V m2:−V；纯自转 ⇒ m1:+V m2:+V（与相位 6 实测符号组一致）
 *
 * 单位一律定点：角度 = 千分之一度，角速度 = 千分之一度/秒。
 * 不用浮点 —— %f 在本 IDF 下可能印不出来（见过），控制环里定点也更省心。
 *
 * ⚠ 线程契约：**全工程只允许一个任务调这些函数**（现在只有 pad_input.c 的 m2c_task）。
 *   TWAI 的发送是"发完再 wait_all_done"，本质是阻塞式的；BLE host 任务
 *   （core 0、4 KB 栈）里绝对不能碰它。手柄掉线不要在这里调 motor_stop()，
 *   走 pad_input_center() 让控制环自己看到 ω_ref=0。
 */
#ifndef MOTOR_TWAI_H
#define MOTOR_TWAI_H

#include <stdbool.h>
#include <stdint.h>

/* 建 TWAI 节点并使能（GPIO4→收发器 T、GPIO5→R，500 kbps）。
 * 只建一次；重复调用返回当前状态，不会泄漏节点。 */
bool motor_init(void);

/* 失能并删节点。之后 motor_ready() 为 false，所有操作返回 false。 */
void motor_deinit(void);

bool motor_ready(void);

/* 两台一起使能/失能（F3 AB 01/00）。上掉电后电机会回到失能态，必须先使能。 */
bool motor_enable(bool on);

/* 世界角速度 → 两台转速指令。acc=0 直起直停（"松手即停"要的就是这个）。
 * wt/wp 为 0 就是停车，但语义上的停车请用 motor_stop()。 */
bool motor_diff_drive(int32_t wt_mdps, int32_t wp_mdps, uint8_t acc);

/* 显式停车：两台速度给 0。比 motor_diff_drive(0,0,0) 更能表达意图。 */
bool motor_stop(void);

/* 读单台实时位置（36，5.5.13），单位千分之一度。addr 可直接当地址探活。 */
bool motor_read_pos(uint8_t addr, int32_t *mdeg);

/* 读两台并差速解算成世界姿态：tilt=(θ1+θ2)/2，pan=(θ1−θ2)/2。 */
bool motor_read_pose(int32_t *tilt_mdeg, int32_t *pan_mdeg);

#endif /* MOTOR_TWAI_H */
