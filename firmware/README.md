# ESP32-S3 固件（ESP-IDF）

> **当前状态：手柄链路（M2a–M2c）已在实物上跑通**，代码在 [`esp32_turret/`](esp32_turret/)。
> 它现在是一个**单文件原型**（基于 ESP-IDF `blecent` 示例裁剪），尚未拆成下面规划的组件结构 ——
> 拆分安排在 CAN 接入之后，避免边验证边重构。
> 完整需求与决策理由见 [`docs/00-总体方案-PRD.md`](../docs/00-总体方案-PRD.md) 第 4 章。

---

## 现在能跑什么

| 阶段 | 内容 | 状态 |
|---|---|---|
| M2a | 扫描 CodexPad-S10，从**扫描响应**里解出按键位图（17 键全通） | ✅ 实物验证 |
| M2b | 连接 + 遍历 GATT + 订阅 `0xFFA1` notify，断线自动重连 | ✅ 实物验证 |
| M2c | 摇杆归一化 → `ω_ref`（±60 °/s）+ 死区，真实掉线即回中 | ✅ 实物验证 |
| M2d | TWAI 收发 → 差速反解 → CAN 下发 | ⬜ 未开始 |

### ⚠ 实测推翻的两处早期设计

细节与实测记录见 [`docs/03-BLE手柄接入执行文档.md`](../docs/03-BLE手柄接入执行文档.md)。

1. 手柄**不是** BLE-HID，走厂商自定义 GATT（`0xFFA0` 输入服务 / `0xFFA1` 通知特征）。
   所以要用 **NimBLE GATT 客户端**，**不是** `esp_hid_host`。
2. notify 载荷是**裸 8 字节** —— `buttons(u32 LE) + Lx + Ly + Rx + Ry`，
   **没有** `0xAA…0x55` 帧头、**没有**转义、**没有** CRC。长度不等于 8 即丢弃。
   并且它是**变化驱动**的：动的时候约 32 Hz，**手一停一包都不发**。
   → 控制环绝不能把"多久没收到包"当成掉线判据；失效判据挂在 GAP 的
   `DISCONNECT` 事件上，没新包 = 状态不变、继续用上一次的值。
   （误停比不响应危险得多。）

### 编译

> ## ⚠ 必须把工程拷到**纯英文路径**再编译
>
> ESP-IDF 的工具链在**字节层**处理不了非 ASCII 路径，**本仓库所在的目录名是中文，
> 在仓库里直接 `idf.py build` 一定失败**。实测（2026-09-19）会在两个地方先后倒下：
>
> ```
> # 先卡在 kconfig（Python 用 GBK 读 build/config.env）
> UnicodeDecodeError: 'gbk' codec can't decode byte 0xaf in position 2189
> Failed to run kconfgen
>
> # 加 PYTHONUTF8=1 能骗过上面这关，但随后 Ninja 自己崩
> terminate called after throwing an instance of 'std::filesystem::__cxx11::filesystem_error'
>   what():  filesystem error: Cannot convert character sequence: Illegal byte sequence
> ```
>
> 所以流程是：**代码在这里改，编译在别处做**。
>
> ```bash
> # 1) 拷到纯英文路径（示例）
> cp -r firmware/esp32_turret /d/esp/esp32_turret
>
> # 2) 在那边编译
> cd /d/esp/esp32_turret
> . $IDF_PATH/export.sh            # Windows 下的激活方式见执行文档 §1.1
> idf.py build
> idf.py -p COM7 flash monitor
> ```
>
> `sdkconfig.defaults` 里已配好目标芯片（esp32s3）、16 MB Flash、OPI PSRAM 与 NimBLE，
> 首次编译不需要再 `idf.py set-target`。

---

## 为什么是 ESP32-S3 + ESP-IDF

| 需求 | 理由 |
|---|---|
| 100 Hz 硬实时增稳环 | FreeRTOS 双核 + 任务绑核，能保证 10 ms 周期 |
| BLE 主机 | `nimble`（ESP-IDF 官方 BLE 协议栈），手柄直连不用外挂模块 |
| CAN | 片内 TWAI 控制器（**只差一颗收发器**） |
| IMU | UART1 = GPIO17/18，汇电籽-601 模块主动上报 100 Hz |
| 视觉链路 | 对外 UART2（**引脚待上板确认**，建议 GPIO10/11） |

详细备选方案对比见 PRD [6.1](../docs/00-总体方案-PRD.md)（为什么不用 STM32H743）、
[6.2](../docs/00-总体方案-PRD.md)（为什么走 CAN）、[6.3](../docs/00-总体方案-PRD.md)（为什么不用 Arduino/MicroPython）。

---

## 目录结构：现状 vs 目标

**现状**（`esp32_turret/`）—— M2a/M2b/M2c 全部挤在 `main/main.c` 里：

```
esp32_turret/
├── CMakeLists.txt
├── sdkconfig.defaults            # 目标芯片 / 16 MB Flash + OPI PSRAM / NimBLE
└── main/
    ├── main.c                    # 扫描 + 连接 + 订阅 + 载荷解析 + ω_ref 映射
    ├── blecent.h                 # blecent 示例自带的广播解析工具
    ├── Kconfig.projbuild         # 沿用示例的 CONFIG_EXAMPLE_* 开关
    └── idf_component.yml         # 依赖 IDF 自带示例组件 nimble_central_utils
```

**目标**——下面这张是拆分后的样子，**尚未落地**：

```
esp32_turret/
├── CMakeLists.txt
├── sdkconfig.defaults            # TWAI / NimBLE(BLE central) / UART / PSRAM 关键配置
├── main/
│   ├── app_main.c                # 初始化 + 创建任务
│   └── Kconfig.projbuild         # 机械与传感器参数（镜像、限位、增益、滤波）
└── components/
    ├── motor_can/                # Emm CAN 协议层（移植自 tools/zdt_can.py）
    │   ├── emm_protocol.c/.h     # 打包 / 解包 / 校验 / 分包重组
    │   └── motor_can.c/.h        # TWAI 驱动 + 收发队列
    ├── imu_uart601/              # UART 帧解析 + 校验 + 角度换算
    ├── stabilizer/               # 速率稳定环 + 使能/旁路 + 自激保护
    ├── gimbal_kin/               # 差速运动学正反解 + 镜像补偿 + 限幅
    ├── motion/                   # 模式状态机 + 指向环 + 轨迹
    ├── gamepad_ble/              # NimBLE GATT 客户端：连接 / 订阅 / 重连
    │   └── codexpad_codec.c/.h   # ★ 载荷解析：裸 8 字节，直接 memcpy，无帧格式
    ├── link_uart/                # 与泰山派的 UART 协议（二期）
    └── safety/                   # 心跳 / 软限位 / 急停 / 故障 / IMU 异常
```

### 移植要点

`components/motor_can/emm_protocol.*` 就是 [`tools/zdt_can.py`](../tools/zdt_can.py)
里 `build_frames()` / `reassemble()` 的 C 版本。帧格式已在 PC 端逐字节实测确认
（扩展帧 `ID = (addr << 8) | 包号`、地址只在 ID 里、校验 `0x6B`、长命令按 7 字节分包），
**照搬即可，不需要重新逆向**。

`components/imu_uart601/` 的协议规格见
[`docs/02-601串口陀螺仪协议.md`](../docs/02-601串口陀螺仪协议.md) ——
那份笔记里的 Python 参考实现带**帧级自检**，C 版按同样逻辑写即可。
两个容易写错的点已经写在里面：校验和不含帧头、`0x0B` 之后还要发 `0x0A` 才开始上报。

---

## 计划中的任务划分

| 任务 | 优先级 | 周期 | 职责 |
|---|---|---|---|
| `imu_task` | **最高** | **事件驱动（100 Hz，UART 收帧触发）** | 解析汇电籽-601 上报帧 → 校验和/超时检查 → 角度换算（×100 整数）→ 写队列 |
| `stabilizer_task` | **最高** | **10 ms** | 速率稳定环：读陀螺 + `ω_ref` → 输出 `ω_cmd` |
| `safety_task` | 高 | 10 ms | 心跳超时、软限位、急停、IMU 异常、故障状态机 |
| `motor_tx_task` | 高 | **10 ms** | CAN 速度模式打包下发（两帧背靠背，无需同步广播，与增稳环同周期） |
| `motion_task` | 中 | 10 ms | 模式状态机、**指向环**、差速反解、限幅 |
| `can_rx_task` | 中 | 事件驱动 | CAN 接收 → 队列（应答 / 到位 / 故障标志） |
| `gamepad_task` | 中 | 事件驱动（每收到一帧 notify） | 手柄 notify 帧 → 摇杆 / 按键状态 |
| `uart_comm_task` | 低 | 20 ms | 与泰山派收发、状态上报（二期） |

**两条关键实践**：

1. **绑核**：ESP32-S3 是双核。把 `imu_task` 和 `stabilizer_task` 绑到 **APP_CPU（核心 1）**，
   把 BLE / `uart_comm_task`（泰山派）/ 协议解析放到 **PRO_CPU（核心 0）**——
   无线协议栈的长临界区会阻塞稳定环。
   注意 **IMU 那路 UART 属于控制通路**，收帧解析要留在核心 1 跟着 `imu_task` 走，
   不能因为"是 UART"就挪到核心 0。
2. **覆盖式队列**：所有跨任务数据用长度 1 的覆盖式队列（`xQueueOverwrite`）。
   实时控制里"用最新数据"永远优于"处理完积压的旧数据"，
   队列积压意味着相位滞后，是稳定环振荡的常见根因。

---

## 计划中的模式状态机

```
             ┌──────────┐   手柄连接 + 使能       ┌───────────────────┐
             │   IDLE   │────────────────────────►│      MANUAL       │
             │ 电机失能 │                         │ 摇杆 → ω_ref      │
             └────┬─────┘                         │ 增稳环可开 / 可关  │
                  │                               └─────────┬─────────┘
                  │                                         │  ▲
                  │                            模式切换键    │  │ 模式切换键
                  │                                         ▼  │
                  │                               ┌───────────────┐
                  │                               │     AUTO      │
                  │                               │ 视觉偏差       │
                  │                               │ → ω_ref       │
                  │                               └───────┬───────┘
                  └──────── 急停 / 故障 ◄──────────────────┘
                            （任意模式 → IDLE + 报警）
```

**稳定环是贯穿 MANUAL 与 AUTO 的公共层**，不随模式切换改变；
模式切换改变的只是"谁提供 `ω_ref`"。

| 模式组合 | 行为 |
|---|---|
| MANUAL + 增稳关 | 纯速度跟随——**用于对比演示** |
| MANUAL + 增稳开 | 陀螺环保证速率跟随并抗扰——**端起来走动时视轴保持指向** |
| AUTO + 增稳开 | 视觉给 `ω_ref` 锁目标，增稳环抵消载体扰动——**真实光电转台的工作模式** |
| AUTO + 增稳关 | 纯视觉伺服——抖动会被放大，仅作对照 |

**AUTO 降级**：泰山派心跳超时 → 若 IMU 正常则**降级为惯性稳定保持**（视轴停在当前惯性指向）；
IMU 也异常才退回 IDLE。**绝不在失去控制源时继续运动。**

---

## 已知的最大风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| ~~手柄 BLE 链路打通~~ | **已消除（2026-09-19）**。M2a–M2c 实测跑通：连接约 260 ms、GATT 表遍历完整、订阅 `0xFFA1` 成功、断线自动重连、摇杆映射到 `ω_ref`。过程见 [`docs/03-BLE手柄接入执行文档.md`](../docs/03-BLE手柄接入执行文档.md) | 遗留：`0xFFE1` 这个未知特征（读写+notify，疑似控制点/震动反馈）未解，但**不阻塞**任何后续里程碑 |
| **IMU 布线跨转动关节** | IMU 装在托盘上，UART 4 线（5V/GND/TX/RX）要跨关节 | 一期用"限位自转 ±90°"规避；必须有掉线检测（帧超时 + 校验和错误计数），异常自动旁路稳定环 |
| **IMU 采样率上限 100 Hz** | 汇电籽-601 固定 100 Hz 上报、协议不支持改速率，增稳闭环带宽目标只能定 **≥ 10 Hz**（原计划 ≥ 20 Hz） | 惯性增稳的任务是"提高阻尼"不是"提高增益"，10 Hz 足够抑制手抖与行走扰动；不足时优先靠机械刚度与低通滤波，而不是硬提增益 |
| **视觉模型工作量** | 采集/标注/训练/RKNN 转换是最大的一块 | 拆成"先用传统图像处理（色块）跑通整条链路 → 再换训练好的模型"，让链路与算法解耦验证 |

---

## 下一步（M2d）

手柄这一环已经闭环，下一棒是**让电机真的动起来**，第一个里程碑是**单轴动起来**：

1. **TWAI 自检**：不接总线，先用内部回环（loopback）验证收发通路，确认引脚与驱动配置无误
2. 接 CAN 收发器（SN65HVD230 / TJA1051 一类，3.3 V）到 TWAI 引脚，
   移植 `emm_protocol`，发一条 `36` 读位置并校验应答
3. 位置模式点位控制，单轴转 30° 再转回来
4. 接上第二台，验证差速运动学和 2 号电机镜像补偿（参考 [`tools/gimbal.py`](../tools/gimbal.py)）
5. 把 `ω_ref` 接进差速反解，手柄真正驱动转台

> ESP32-S3 **片内就有 TWAI 控制器**，外接的只是一颗物理层收发器。
> 注意接线是**直连**（TXD→TWAI_TX、RXD→TWAI_RX），**不像 UART 那样交叉**；
> 收发器的 RS 脚要接地（高速模式）。
