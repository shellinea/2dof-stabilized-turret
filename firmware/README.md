# ESP32-S3 固件（ESP-IDF）

> **当前状态：整条「手柄 → BLE → `ω_ref` → 差速逆解 → CAN → 两台电机」链路
> 已在实物上跑通（M2a–M2e，2026-09-21）**，代码在 [`esp32_turret/`](esp32_turret/)。
> 已按职责拆成 6 个模块（见下面「目录结构」），BLE 部分基于 ESP-IDF `blecent` 示例裁剪，
> CAN 部分从 7 个相位的 TWAI 自检固件里抽出**已验证**的代码。
> 下一棒是 IMU 增稳环，届时再按 `components/` 拆分。
> 完整需求与决策理由见 [`docs/00-总体方案-PRD.md`](../docs/00-总体方案-PRD.md) 第 4 章。
>
> CAN 侧的接线、Emm 帧格式、TWAI 陷阱与线程契约见
> [`docs/04-ESP32-CAN电机驱动.md`](../docs/04-ESP32-CAN电机驱动.md)。

---

## 现在能跑什么

| 阶段 | 内容 | 状态 |
|---|---|---|
| M2a | 扫描 CodexPad-S10，从**扫描响应**里解出按键位图（17 键全通） | ✅ 实物验证 |
| M2b | 连接 + 遍历 GATT + 订阅 `0xFFA1` notify，断线自动重连 | ✅ 实物验证 |
| M2c | 摇杆归一化 → `ω_ref`（±60 °/s）+ 死区，真实掉线即回中 | ✅ 实物验证 |
| M2d | TWAI 收发 + Emm 协议 + 差速逆解（相位 1~7 自检固件全通） | ✅ 实物验证 |
| M2e | `ω_ref` 换成手柄真值，手柄驱动转台 | ✅ 实物验证（2026-09-21） |

### 实测推翻的三处早期设计

细节与实测记录见 [`docs/03-BLE手柄接入执行文档.md`](../docs/03-BLE手柄接入执行文档.md)
与 [`docs/04-ESP32-CAN电机驱动.md`](../docs/04-ESP32-CAN电机驱动.md)。

1. 手柄**不是** BLE-HID，走厂商自定义 GATT（`0xFFA0` 输入服务 / `0xFFA1` 通知特征）。
   所以要用 **NimBLE GATT 客户端**，**不是** `esp_hid_host`。
2. notify 载荷是**裸 8 字节** —— `buttons(u32 LE) + Lx + Ly + Rx + Ry`，
   **没有** `0xAA…0x55` 帧头、**没有**转义、**没有** CRC。长度不等于 8 即丢弃。
   并且它是**变化驱动**的：动的时候约 32 Hz，**手一停一包都不发**。
   → 控制环绝不能把"多久没收到包"当成掉线判据；失效判据挂在 GAP 的
   `DISCONNECT` 事件上，没新包 = 状态不变、继续用上一次的值。
   （误停比不响应危险得多。）
3. **「运动学是现成的、那是搬运」不成立**。PC 端确实验过差速逆解和 CAN 帧，
   但 C 侧要从零写一遍，并且要在电机上**重新验一次**（位置模式、速度模式各一遍）。
   搬运的部分只有帧格式本身。

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
> # 1) 拷到纯英文路径（示例；2026-09-21 的 M2e 合并就是在 D:\esp\turret_repo 里编的）
> cp -r firmware/esp32_turret /d/esp/turret_repo
>
> # 2) 在那边编译
> cd /d/esp/turret_repo
> . $IDF_PATH/export.sh            # Windows 下的激活方式见执行文档 §1.1
> idf.py set-target esp32s3        # sdkconfig.defaults 已配好，换机器时跑一次
> idf.py build
> idf.py -p COM7 flash monitor
> ```
>
> `sdkconfig.defaults` 里已配好目标芯片（esp32s3）、16 MB Flash、OPI PSRAM 与 NimBLE。
> `main/CMakeLists.txt` 里**必须**显式列 `REQUIRES`：根 `CMakeLists.txt` 开了
> `MINIMAL_BUILD ON`，不会自动带依赖（`bt` / `nvs_flash` / `esp_timer` / `esp_driver_twai`）。
>
> 已编译验证（2026-09-21，ESP-IDF v5.5.5 / esp32s3）：**936 项全部通过、无 warning**，
> 应用分区 0x82510 字节（占用 35%）。

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

**现状**（`esp32_turret/`）—— 手柄链路 + 电机链路按职责拆成 6 个模块，都在 `main/` 下：

```
esp32_turret/
├── CMakeLists.txt
├── sdkconfig.defaults            # 目标芯片 / 16 MB Flash + OPI PSRAM / NimBLE
└── main/
    ├── app_main.c                # 入口：NVS + NimBLE 初始化 + motor_bringup() 电机上电自检
    ├── codexpad.h                # 公共接口：UUID / 状态结构 / 键位表 / 跨模块 API
    ├── pad_scan.c                # M2a：广播扫描 + 扫描响应解析
    ├── pad_link.c                # M2b：GAP —— 扫描 / 连接 / 断线重扫 + 事件总回调
    ├── pad_gatt.c                # M2b：GATT —— 服务链发现 + 订阅 notify
    ├── pad_input.c               # M2b/M2c/M2e：notify → 覆盖队列 → ω_ref → 差速逆解
    ├── motor_twai.c / .h         # M2d/M2e：TWAI 驱动 + Emm 协议 + 差速逆运动学
    └── idf_component.yml         # 依赖 IDF 自带示例组件 nimble_central_utils
```

> 手柄那 5 个文件拆分时**只搬位置、只删死代码，没有改逻辑**。删掉的是 blecent 示例自带的
> ANS 通知演示链（read/write/subscribe 九件套）、`should_connect` 系列、EATT / 加密 / 扩展广播
> 等编译期就关闭的分支，以及一段针对**实测并不存在的** `0xAA…0x55` 帧格式写的 CRC
> 兜底解析。拆分后固件比原来小 224 字节；上板实测与拆分前一致
> （`val_handle=33` / `CCCD=34` / `MTU=256`，摇杆映射 ±60 °/s 正常）。
>
> `motor_twai.c` 不是新写的，是从 7 个相位的 TWAI 自检固件里**抽出已验证的部分**：
> Emm 帧格式/拆包、`0x36` 读位置、`F6` 速度模式、差速逆解都在相位 1~7 里跑过
> （实测记录见 [`docs/04-ESP32-CAN电机驱动.md`](../docs/04-ESP32-CAN电机驱动.md) §12）。

**目标**——下面这张是**手柄之外**的组件拆分，**尚未落地**：

```
esp32_turret/
├── CMakeLists.txt
├── sdkconfig.defaults            # TWAI / NimBLE(BLE central) / UART / PSRAM 关键配置
├── main/
│   ├── app_main.c                # 初始化 + 创建任务
│   ├── pad_*.c / codexpad.h      # 手柄链路（已落地，L4 之后再挪进 components/）
│   ├── motor_twai.c / .h         # 电机链路（已落地，同样等 L4 再挪进 components/）
│   └── Kconfig.projbuild         # 机械与传感器参数（镜像、限位、增益、滤波）
└── components/
    ├── imu_uart601/              # UART 帧解析 + 校验 + 角度换算
    ├── stabilizer/               # 速率稳定环 + 使能/旁路 + 自激保护
    ├── motion/                   # 模式状态机 + 指向环 + 轨迹
    ├── link_uart/                # 与泰山派的 UART 协议（二期）
    └── safety/                   # 心跳 / 软限位 / 急停 / 故障 / IMU 异常
```

> `motor_can/` 那两个文件**已经以 `motor_twai.c/.h` 的形式落地了**（在 `main/` 下），
> 所以从"目标树"里划掉。剩下没落地的是 IMU / 增稳 / 安全那几块。

### 移植要点

`motor_twai.c` 里 Emm 的打包/拆包就是 [`tools/zdt_can.py`](../tools/zdt_can.py)
里 `build_frames()` / `reassemble()` 的 C 版本。帧格式已在 PC 端逐字节实测确认
（扩展帧 `ID = (addr << 8) | 包号`、地址只在 ID 里、校验 `0x6B`、长命令按 7 字节分包），
**照搬即可，不需要重新逆向**。
C 版比 Python 版多一个**只有写 C 才会踩的坑**：请求帧自带 `0x6B` 时不能再走
会自动补校验的那条路径，否则补成 `36 6B 6B`，电机静默丢弃 ——
完整说明见 [`docs/04-ESP32-CAN电机驱动.md`](../docs/04-ESP32-CAN电机驱动.md) §3。

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
| ~~CAN 驱动 + 差速逆解~~ | **已消除（2026-09-20/21）**。相位 1~7 自检全通，手柄驱动转台上板跑通。接线 / Emm 帧格式 / TWAI 陷阱见 [`docs/04-ESP32-CAN电机驱动.md`](../docs/04-ESP32-CAN电机驱动.md) | 遗留：位置到达窗口 `D1 07` 在本机电机固件 V2.0.4 上是死命令，只能用小屏改 `PRWindow`；只影响位置模式收尾，速度模式主线无此问题 |
| **CAN 总线会静默丢帧** | 实测（相位 7）遇到过一轮 2 号电机的 `FD` 没收到，总线上同时有填充错误 / 仲裁丢失。**丢帧不报错、不重传** | 当前 20 ms 控制环 + 200 ms 心跳兜底（最多多转 200 ms）；接 100 Hz 增稳环前**必须**加应答核对或看门狗，否则"发出去电机没收到"直接变成控制失效 |
| **IMU 布线跨转动关节** | IMU 装在托盘上，UART 4 线（5V/GND/TX/RX）要跨关节 | 一期用"限位自转 ±90°"规避；必须有掉线检测（帧超时 + 校验和错误计数），异常自动旁路稳定环 |
| **IMU 采样率上限 100 Hz** | 汇电籽-601 固定 100 Hz 上报、协议不支持改速率，增稳闭环带宽目标只能定 **≥ 10 Hz**（原计划 ≥ 20 Hz） | 惯性增稳的任务是"提高阻尼"不是"提高增益"，10 Hz 足够抑制手抖与行走扰动；不足时优先靠机械刚度与低通滤波，而不是硬提增益 |
| **视觉模型工作量** | 采集/标注/训练/RKNN 转换是最大的一块 | 拆成"先用传统图像处理（色块）跑通整条链路 → 再换训练好的模型"，让链路与算法解耦验证 |

---

## 下一步（IMU 增稳环）

手柄这一环已经闭环，电机也已经被手柄驱动起来了（2026-09-21 上板确认）。
下一棒是**惯性增稳**，即把 `m2c_task` 从"摇杆直通"扩成"手柄目标 + IMU 反馈"的复合环：

1. **接汇电籽-601**：UART1 = GPIO17/18，5 V。协议规格见
   [`docs/02-601串口陀螺仪协议.md`](../docs/02-601串口陀螺仪协议.md)，
   帧级自检脚本在文档里，可直接跑。
2. **`imu_task`**（事件驱动，100 Hz 收帧触发）：解析 → 校验和/超时检查 → 角度换算（×100 整数）→ 覆盖队列。
3. **`stabilizer_task`**（10 ms）：读陀螺 + `ω_ref` → 输出 `ω_cmd`。
4. **必须补的电机侧保障**：总线会**静默丢帧**（实测见过 2 号一整轮 `FD` 没收到），
   100 Hz 连续发速度指令时**要加应答核对或看门狗**，否则"发出去电机没收到"
   会直接变成控制失效。这是接增稳环的前置条件，不是可选项。
5. 增稳环与手柄目标**共用**同一条 `motor_diff_drive()` 出口（线程契约只允许
   `m2c_task` 调，见 [`docs/04`](../docs/04-ESP32-CAN电机驱动.md) §6）。

> ⚠ **`OMEGA_MAX_DPS = 60 °/s` 与各轴符号尚未钉死**：相位 7 已给出符号基准
> `cmd1 = ω_tilt + ω_pan`，真要反了只需把某个轴取负，不用改结构。
> 上板觉得手感不对再调 —— 这是当前唯一已知的未定项。
