# 02 - BLE 手柄接入执行文档

**里程碑 M2：打通「CodexPad-S10 手柄 → ESP32-S3」的蓝牙输入链路**

| 项 | 内容 |
|---|---|
| 版本 | v1.0（2026-09-17） |
| 关联 | `docs/00-总体方案-PRD.md` §5.4、§4.1.2、§4.1.3、里程碑 M2 |
| 目标产物 | ESP32-S3 作为 BLE 主机连接手柄，实时解析 17 键 + 双摇杆，映射为 `ω_ref` 并验证 |
| 前置硬件 | ESP32-S3（YD-ESP32-S3-N16R8）+ CodexPad-S10 手柄 + USB 线 |
| **不需要** | CAN 收发器、电机、泰山派、IMU —— 本里程碑与它们完全解耦 |

---

## 0. 为什么先做这一段

1. **它是全案风险最高的一环**（PRD R-01）。手柄能不能连上、report 怎么解析，是唯一可能把一期推翻重来的事。
2. **它不依赖任何还没买/没定的东西**。BLE 走芯片内部射频，一个 GPIO 都不用；CAN 收发器、IMU 走线都还没落地，但这一段现在就能做完。
3. **做完它，剩下的一期只是"把已知的数搬到已知的协议上"**：摇杆 → 运动学 → CAN 帧（`tools/zdt_can.py` 已经验证过的格式）。难度是递减的。

---

## 1. ⚠ 重要更正：这不是标准 BLE-HID 手柄

**PRD §5.4 的假设是错的，本文件以此为准。**

CodexPad-S10 官方 README 原文：

> **⚠️ 这不是一款"即插即用"的通用游戏手柄**
> CodexPad-S10 **不是 BLE-HID 设备**，它不会像 Xbox、PlayStation 等标准手柄那样被操作系统自动识别为标准游戏控制器。
> **❌ 不能**直接连上 Windows / macOS / Linux 或手机、游戏主机就去打游戏；
> **✅ 必须**通过**代码手动建立蓝牙连接**，并自行解析手柄上报的数据（按键、摇杆等）来获取输入。

| 影响 | 说明 |
|---|---|
| **不能用 `esp_hid_host`** | PRD §4.1.2 里 `gamepad_ble/` 用 `esp_hid_host` 的写法作废 |
| **改用 NimBLE GATT 客户端** | ESP32-S3 主动连手柄的自定义 GATT 服务，订阅通知特征，自己解析二进制帧 |
| **好消息** | 厂商协议**是公开的**（开源库 + 手册齐全），我们不需要猜。第 2 章已经把它完整逆向出来了 |
| **另一个好消息** | "自定义协议"比"HID report 描述符"**更好处理** —— 帧格式固定、按键位固定，不像 HID 那样每个厂商一套描述符 |

> 备选路线（如果 NimBLE 移植卡住）见第 8 章。

---

## 2. 协议情报（已从官方开源库确认）

> 来源：`github.com/CodexPad/` 下的 `codex_pad_s10`、`codex_pad_guide`、`codex_pad_arduino_lib`、
> `gamepad_codec_arduino_lib`、`robust_frame_arduino_lib`、`gamepad_input_arduino_lib`。
> 以下所有常量都是从这些库的源码里逐条读出来的，**可直接作为实现依据**。

### 2.1 设备与连接

| 项 | 值 |
|---|---|
| 广播名称 | `CodexPad-` 开头（如 `CodexPad-XXXX`） |
| 蓝牙版本 | BLE 5.3，**仅作从机（peripheral）** |
| 发射功率 | −16 dBm ~ +6 dBm，可通过 GATT 写（服务 `0x1804` / 特征 `0x2A07`） |
| BD_ADDR | 印在**手柄背面中央标签**上，格式 `XX:XX:XX:XX:XX:XX`（如 `E4:66:E5:A2:24:5D`） |
| 输入规格 | **17 个按键 + 2 个双轴模拟摇杆**，摇杆 8 位分辨率（0 ~ 255） |
| 开机 | 短按中央 **Home 键**，1 号蓝灯慢闪（约 1 秒亮/灭）= 正在广播 |
| 已连接 | 1 号蓝灯**常亮** |
| ⚠ **广播超时自动关机** | 开机后持续慢闪 **超过 1 分钟**无设备连接 → 自动关机。**开发时最容易踩这个坑** |
| ⚠ **Home 键长按 3 秒 = 关机** | 所以 **Home 键绝对不能拿来做急停或任何游戏按键** |

### 2.2 GATT 结构与 UUID

| 服务 | UUID | 特征 | UUID | 用途 |
|---|---|---|---|---|
| **输入服务** | **`0xFFA0`** | **输入特征** | **`0xFFA1`** | **notify，输入数据流全在这里** |
| 通用访问 GAP | `0x1800` | 设备名 | `0x2A00` | 读设备名 |
| 设备信息 | `0x180A` | 型号 / 序列号 / 固件版本 | `0x2A24` / `0x2A25` / `0x2A26` | 读固件版本 |
| 电池 | `0x180F` | 电量 | `0x2A19` | 读电量 |
| 发射功率 | `0x1804` | 发射功率 | `0x2A07` | 可写 |

**连接流程（照抄官方 `codex_pad.cpp` 的顺序）**：

1. BLE 主机初始化（设备名任意，官方用 `CodexPadClient`）
2. 连接目标地址
3. 读 `0x1800/0x2A00`（设备名）、`0x180A/0x2A24`（型号）、`0x180A/0x2A26`（固件版本，3 字节）
4. `getService(0xFFA0)` → `getCharacteristic(0xFFA1)` → 确认 `canNotify()` → **订阅 notify**
5. 收到通知 → 解析帧（第 2.3、2.4 节）
6. **不需要配对/绑定**，直接连接即可

### 2.3 帧格式（`robust_frame` 库）

```
┌────────┬──────────────────┬────────────────┬────────┐
│  0xAA  │  转义后的载荷      │  转义后的CRC8   │  0x55  │
│  1B    │  1 ~ 2n B        │  1 ~ 2B        │  1B    │
└────────┴──────────────────┴────────────────┴────────┘
```

- 帧头 `0xAA`，帧尾 `0x55`，转义符 `0xDB`，转义异或值 `0x20`
- **转义规则**：载荷/CRC 里凡出现 `0xAA`、`0x55`、`0xDB` 任一字节 → 替换为 `0xDB` + `(原字节 ^ 0x20)`
- **CRC8**：`poly = 0x1D, init = 0xFF, xorout = 0xFF`（MSB-first，即 **CRC-8/SAE-J1850**）
  - 官方实现是 256 项查表：`crc = 0xFF; for each byte: crc = table[crc ^ byte]; return crc ^ 0xFF;`
  - **最稳的做法是直接移植官方那张 256 项表**（`robust_frame_arduino_lib/src/crc8.h`），避免多项式细节搞错
- 接收侧按**状态机**逐字节处理：找帧头 → 收数据 → 遇 `0xDB` 进转义态 → 遇 `0x55` 收尾并校验 CRC

### 2.4 载荷格式（`gamepad_codec` + `gamepad_input` 库）

载荷 = 1 字节数据类型 + 8 字节状态结构体，**共 9 字节**：

```
┌───────────────┬──────────────────┬────┬────┬────┬────┐
│ DataType=0x01 │  buttons (u32 LE) │ Lx │ Ly │ Rx │ Ry │
│     1B        │       4B          │ 1B │ 1B │ 1B │ 1B │
└───────────────┴──────────────────┴────┴────┴────┴────┘
```

C 结构体（直接对应，注意 packed）：

```c
#define CODEXPAD_DATATYPE_INPUT_STATE  0x01

typedef struct __attribute__((packed)) {
    uint32_t buttons;    // 小端
    uint8_t  axes[4];    // 顺序固定：Lx, Ly, Rx, Ry；中心值 0x80
} codexpad_state_t;      // sizeof == 8

#define CODEXPAD_AXIS_LEFT_X   0
#define CODEXPAD_AXIS_LEFT_Y   1
#define CODEXPAD_AXIS_RIGHT_X  2
#define CODEXPAD_AXIS_RIGHT_Y  3
#define CODEXPAD_AXIS_CENTER   0x80
```

### 2.5 按键位表（`buttons` 位域，共 17 键）

| 位 | 按键 | 位 | 按键 |
|---|---|---|---|
| `1<<0` | 上（十字键 Up） | `1<<9`  | L2 |
| `1<<1` | 下（Down） | `1<<10` | L3（左摇杆按下） |
| `1<<2` | 左（Left） | `1<<11` | R1 |
| `1<<3` | 右（Right） | `1<<12` | R2 |
| `1<<4` | □ Square/X | `1<<13` | R3（右摇杆按下） |
| `1<<5` | △ Triangle/Y | `1<<14` | Select |
| `1<<6` | ✕ Cross/A | `1<<15` | Start |
| `1<<7` | ○ Circle/B | `1<<16` | **Home（⚠ 长按关机，勿作功能键）** |
| `1<<8` | L1 | | |

```c
#define BTN_UP       (1u << 0)
#define BTN_DOWN     (1u << 1)
#define BTN_LEFT     (1u << 2)
#define BTN_RIGHT    (1u << 3)
#define BTN_SQUARE_X (1u << 4)
#define BTN_TRIANGLE_Y (1u << 5)
#define BTN_CROSS_A  (1u << 6)
#define BTN_CIRCLE_B (1u << 7)
#define BTN_L1       (1u << 8)
#define BTN_L2       (1u << 9)
#define BTN_L3       (1u << 10)
#define BTN_R1       (1u << 11)
#define BTN_R2       (1u << 12)
#define BTN_R3       (1u << 13)
#define BTN_SELECT   (1u << 14)
#define BTN_START    (1u << 15)
#define BTN_HOME     (1u << 16)   // ⚠ 不要用
```

### 2.6 广播数据里就带按键状态（额外收获）

手柄的**广播 Manufacturer Specific Data** 里包含当前按键状态，所以**不连接也能读到按键**：

```c
#pragma pack(push, 1)
struct ManufacturerSpecificData {
    uint16_t company_id;                      // == 0xFFFF
    uint8_t  header[8];                       // == "CodexPad"
    uint8_t  version_major, version_minor, version_patch;
    uint32_t button_state;                    // 当前按键位，同 2.5 的表
    uint8_t  button_states_duration_seconds;  // 已保持秒数
};
#pragma pack(pop)
```

用途：
1. **实现"按键掩码扫描连接"** —— 官方特色功能：让手柄按住某组合键，主机扫描时只连"按键状态恰好等于掩码"且 RSSI 最高的那台。多手柄环境防误连、可随时换手柄，代码里不用硬编码 BD_ADDR。
2. **M2a 阶段的最小验证** —— 连都不用连，扫到就能确认"板子蓝牙 OK + 手柄在广播 + 协议理解正确"。

---

## 3. 环境准备

> **当前本机状态（2026-09-17 实测）：ESP-IDF 未安装、esptool 未安装、bleak 未安装。Python 3.10.11 已有。**
> 所以下面从零开始。

### 3.1 安装 ESP-IDF（Windows）

两条路，选一条：

- **推荐：乐鑫离线安装器** —— 下载 `ESP-IDF Tools Installer`（Windows 版），勾选 ESP32-S3 支持，
  它会装好工具链 + Python 环境 + VS Code 插件。装完在开始菜单打开 `ESP-IDF PowerShell` / `ESP-IDF CMD`。
- **或：git 克隆 + `install.bat`** —— 体积更大、要自己配环境变量，不如安装器省事。

> ⚠ 装 ESP-IDF 的体积和时间都不小（数 GB，含工具链）。**这一步建议单独留出时间，别和调试混在一起。**

### 3.2 pip 必须走清华镜像（已知坑）

本机 pip 直连 pypi.org 会 TLS 断连（`SSLEOFError`）。所有 pip 安装都加镜像参数：

```bash
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple <包名>
```

### 3.3 板子配置（照抄，别改）

板子型号 **YD-ESP32-S3-N16R8**（16 MB Flash + 8 MB PSRAM），厂商文档明确的两条硬约束：

| 配置 | 值 | 原因 |
|---|---|---|
| 目标芯片 | `esp32s3` | — |
| **PSRAM** | **`OPI PSRAM`（Octal）** | N16R8 的 PSRAM 是八线的。**不选 OPI，PSRAM 用不了**（本项目增稳环要用 PSRAM 放缓冲，现在先配上免得以后返工） |
| Flash 大小 | 16 MB | 记得改分区表，否则默认 2 MB 会发现"代码放不下" |
| ⚠ 禁用引脚 | **GPIO26 ~ GPIO32** | 板载 Flash/PSRAM 专用，**不可作普通 IO**，误用会直接崩 |

`sdkconfig` 关键项：

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
```

### 3.4 串口

- 板载 USB 转串口芯片型号见 `esp32s3/开发板原理图.pdf`，若系统不认串口需装对应驱动（CP210x / CH34x 类）。
- 烧录与日志走同一个口：`idf.py -p COMx flash monitor`（`COMx` 换成实际端口）。
- **本里程碑不用任何 GPIO**，所以不存在和电机/CAN 抢引脚的问题。

### 3.5 （可选但强烈建议）PC 端诊断工具

装 `bleak`（跨平台 BLE 库），配合官方 `codex_pad_bleak_lib`：

```bash
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple bleak
```

用途：**当 ESP32 侧行为不对时，用它来判定"是手柄的问题还是我们代码的问题"**。
如果 PC 上也读不出数据，那就不是 ESP-IDF 代码的锅。这是最省时间的二分法。

---

## 4. 执行步骤

### M2a —— 最小验证：只扫描，不连接

**目的**：用最少代码证明"板子蓝牙正常 + 手柄在广播 + 我们理解对了协议"。

做法：基于 ESP-IDF 的 `examples/bluetooth/nimble/blecent`（NimBLE central 示例）裁剪，
只保留扫描部分，过滤广播名称前缀 `CodexPad-`，把扫到的设备打印出来：

- 设备名、BD_ADDR、RSSI
- **解析广播里的 Manufacturer Specific Data**，打印 `version_*` 和 `button_state`（按 2.6 的表翻译成按键名）

**验收**：手柄开机（蓝灯慢闪）后，1 分钟内串口打印出 `CodexPad-XXXX`，且按下不同按键时打印的按键名跟着变。

> 这一步就已经把 R-01 的大半不确定性消掉了：**能扫到、能读到按键位，说明协议情报无误。**
> ⚠ 记得手柄 **1 分钟不连接会自动关机**，扫描期间若手柄灭了要重新短按 Home 开机。

### M2b —— 连接 + 订阅 + 解析通知

**目的**：拿到连续的摇杆数据。

做法：

1. 用 M2a 拿到的 BD_ADDR（或先用扫描拿到再连）发起连接
2. 依次读设备名 / 型号 / 固件版本（见 2.2 第 3 步）
3. 发现服务 `0xFFA0` → 特征 `0xFFA1` → 校验 `canNotify` → **写 CCCD 订阅 notify**
4. 在 `BLE_GAP_EVENT_NOTIFY_RX` 事件里把收到的字节喂给**帧解析状态机**（2.3 的规则）
5. 解析出完整帧后，校验 CRC8，再读载荷：`DataType` 必须 == `0x01`，后 8 字节按 `codexpad_state_t` 解释
6. 把解析好的 `codexpad_state_t` 丢进 **FreeRTOS 队列**（供后续任务消费）

**帧解析器要点**（逐字节状态机，照 `robust_frame::Parser` 移植）：

```
状态 1 找帧头：byte == 0xAA → 清空缓冲，进状态 2
状态 2 收数据：byte == 0x55 → 进状态 3（校验）
               byte == 0xDB → 进转义态（下一个字节 ^= 0x20 后按普通数据存）
               其他        → 直接存
状态 3 校验：  最后 1 字节是 CRC8，对前面的载荷算 CRC8 比对
               一致 → 一帧完成，交给载荷解析
               不一致 → 丢弃，回状态 1
```

⚠ **转义处理是最容易写错的地方**。CRC 也要在**还原转义之后**的载荷上算。

**验收**：
- 推动任意摇杆，串口连续打印 `Lx/Ly/Rx/Ry`，范围大致覆盖 `0 ~ 255`，**松手回 `0x80` 附近**
- 帧率统计（每秒收到多少完整帧）—— 记下来，这是指向环的输入频率上限（PRD 期望 ≈100 Hz，待实测）
- 连续跑 10 分钟：不丢帧、不重启、不内存泄漏

### M2c —— 映射为控制量 + 状态机

按 PRD §5.4 的约定（左摇杆 → pan 角速度，右摇杆 → tilt 角速度，松手即停）：

```c
// 轴值 0..255，中心 0x80 → 归一化 [-1.0, +1.0]
static inline float axis_norm(uint8_t raw) {
    return ((float)raw - 128.0f) / 127.0f;
}

// 死区：避免摇杆回中不准导致的漂移
#define AXIS_DEADZONE 0.08f
static inline float apply_deadzone(float v) {
    if (v > -AXIS_DEADZONE && v < AXIS_DEADZONE) return 0.0f;
    return v;
}

omega_pan_ref  = apply_deadzone(axis_norm(st.axes[CODEXPAD_AXIS_LEFT_X]))  * OMEGA_MAX;
omega_tilt_ref = apply_deadzone(axis_norm(st.axes[CODEXPAD_AXIS_RIGHT_Y])) * OMEGA_MAX;
```

**待实测确认（不要照抄就信）**：
- 哪个轴对应哪个方向（`axes[]` 的顺序是固定的 Lx/Ly/Rx/Ry，但**物理方向与符号要手动试出来**）
- 是否需要**指数曲线**（小幅慢速、大幅快速），单手操控更舒服
- `OMEGA_MAX` 取多少（等和电机的联动调）—— 本里程碑先打印数值，不下发

**按键映射**（本阶段只打印事件）：

| 按键 | 用途 | 备注 |
|---|---|---|
| 左摇杆 / 右摇杆 | pan ω / tilt ω | 松手即停 |
| **Start** | 模式切换（MANUAL ↔ AUTO，三期） | — |
| **L1 + R1 + Start 组合** | **急停** | 用组合键避免误触 |
| **Home** | ❌ **禁用** | 长按会关机 |

### M2d —— 下一棒（本里程碑结束）

本里程碑的**终点**是：串口能稳定打印解析好的摇杆/按键，`ω_ref` 数值合理。

**再接上电机需要先买 CAN 收发器**（PRD §3.1.3 那唯一需要采购的关键件）。买了之后：

```
摇杆 → ω_ref → 差速逆运动学（tools/gimbal.py 已验证）→ CAN 帧（tools/zdt_can.py 已验证）→ 电机
```

运动学和 CAN 帧格式**都是现成的**，那一步是"搬运"而不是"探索"。

---

## 5. 代码骨架（NimBLE 关键片段）

> 完整样板（BLE 主机初始化、地址获取、事件循环）**直接参照 ESP-IDF 自带的
> `examples/bluetooth/nimble/blecent`**，不要从零写。下面只列本项目特有的部分。

```c
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

// ---- 项目特有常量 ----
#define CODEXPAD_SVC_UUID16   0xFFA0
#define CODEXPAD_CHR_UUID16   0xFFA1

static const ble_uuid16_t kSvcUuid = BLE_UUID16_INIT(CODEXPAD_SVC_UUID16);
static const ble_uuid16_t kChrUuid = BLE_UUID16_INIT(CODEXPAD_CHR_UUID16);

// ---- 帧解析状态机（照 robust_frame::Parser 移植）----
static uint8_t  s_frame_buf[32];
static size_t   s_frame_len;
static bool     s_in_escape;

// CRC8：poly 0x1D / init 0xFF / xorout 0xFF
static uint8_t crc8_calc(const uint8_t *d, size_t n) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; ++i) crc = crc8_table[crc ^ d[i]];  // 移植官方 256 项表
    return crc ^ 0xFF;
}

static void on_frame_complete(const uint8_t *payload, size_t len) {
    if (len < 1 + sizeof(codexpad_state_t)) return;
    if (payload[0] != CODEXPAD_DATATYPE_INPUT_STATE) return;

    codexpad_state_t st;
    memcpy(&st, payload + 1, sizeof(st));   // 结构体已 packed

    // 丢进队列，供 gamepad_task 消费（长度 1 覆盖式队列，永远用最新样本）
    xQueueOverwrite(s_gamepad_q, &st);
}

static void feed_byte(uint8_t b) {
    // ... 按第 2.3 节的状态机实现：
    //     找 0xAA / 处理 0xDB 转义 / 收到 0x55 时校验 CRC8 并调 on_frame_complete
}

// ---- 连接成功后：发现服务/特征并订阅 ----
// ble_gattc_disc_svc_by_uuid  →  ble_gattc_disc_chrs_by_uuid
//   →  找到 BLE_GATT_DSC_CLT_CFG_UUID16（CCCD），写 {0x01,0x00} 开启 notify
// 通知到达：BLE_GAP_EVENT_NOTIFY_RX 事件里取 om->om_data / om->om_len → feed_byte()
```

**实现顺序建议**：先把 `feed_byte()` 写成纯函数，**在 PC 上用 Python 造几帧假数据单测**（按 2.3/2.4 的规则编码），
确认解析正确后再上板。这样能把"解析器写错"和"BLE 连不上"两个问题彻底分开。

---

## 6. 验收标准

| 编号 | 验收项 | 判定 |
|---|---|---|
| AC-BLE-1 | 扫到 `CodexPad-XXXX`，能读出广播按键位 | M2a |
| AC-BLE-2 | 连接成功，手柄 1 号蓝灯**常亮** | M2b |
| AC-BLE-3 | 四个轴数值连续、范围合理、松手回中（`0x80`±死区） | M2b |
| AC-BLE-4 | 17 个按键逐个测试：广播按键位与 notify 解析结果**一致**，无串扰 | M2b |
| AC-BLE-5 | 手柄关机再开机 → ESP32 能自动重连 | M2b |
| AC-BLE-6 | 连续运行 10 分钟：不丢帧、不重启；记录实际帧率 | M2b |
| AC-BLE-7 | `ω_ref` 映射：左摇杆→pan、右摇杆→tilt；松手 `ω_ref` 归零 | M2c |
| AC-BLE-8 | 断掉 BLE（走远/关手柄）→ 系统进安全态，`ω_ref` 归零（对应 SR-05） | M2c |

---

## 7. 排错表

| 现象 | 可能原因 | 处理 |
|---|---|---|
| **扫不到手柄** | 手柄没开机 / 已过 1 分钟自动关机 / 被手机或电脑连走了 | 短按 Home 重新开机，蓝灯慢闪后 1 分钟内扫描；关掉附近已连的蓝牙主机 |
| 扫到但连不上 | BD_ADDR 写错 / RSSI 太差 | 对照背面标签核对地址；靠近板子 |
| 连上又立刻断 | 供电不足（USB 口带不动） / 手柄电量低 | 换 USB 口或带供电的 Hub；给手柄充电 |
| 收到通知但 **CRC 一直校验失败** | ① 转义处理写错（最常见）② CRC 表移植错 ③ 在转义前的数据上算了 CRC | 用 PC 端 `bleak` 抓原始字节流对照；CRC 直接在**还原转义后**的载荷上算 |
| 帧能解但数值乱跳 | 把广播按键位和 notify 数据混用了 | 明确区分两个数据来源 |
| 手柄用一会儿自动关机 | **广播超时**（未连接超 1 分钟）或电量低 | 尽快连接；充电 |
| `idf.py` 找不到 / 编译报错缺 target | ESP-IDF 环境变量没加载 | 用开始菜单的 `ESP-IDF PowerShell`，不要用普通终端 |
| 编译提示空间不足 | Flash 大小没设成 16 MB | 改 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` + 分区表 |
| 行为诡异 / 随机崩溃 | 误用了 GPIO26~32 | 检查引脚，这些是 Flash/PSRAM 专用 |

---

## 8. 风险与备选路线

| 方案 | 做法 | 优点 | 缺点 | 建议 |
|---|---|---|---|---|
| **主线：ESP-IDF + NimBLE** | 自己写 GATT 客户端 + 帧解析 | 项目正式栈，无返工；面试可讲"自己实现 BLE GATT 客户端" | 代码量约 400~600 行，NimBLE C API 较啰嗦 | ✅ **采用** |
| 备选 A：arduino-esp32 当 IDF 组件 | 把 Arduino 作为 IDF component 引入，直接用官方 `CodexPad` 库 | **最快跑通**（30 分钟） | 混框架、体积大、面试讲不清"哪里是你写的" | 卡住时的逃生路线 |
| 备选 B：MicroPython + 官方 `codex_pad_mpy_lib` | 刷 MicroPython 固件跑官方库 | 最快验证链路 | 非项目技术栈，等于白做一遍 | 只用于**临时证伪** |
| 备选 C：PC + `bleak` + `codex_pad_bleak_lib` | 电脑上读手柄 | 不需要板子 | 只是诊断工具 | ✅ **建议先做，作为二分法基准** |
| 备选 D：换标准 BLE-HID 手柄 | 回到 `esp_hid_host` 路线 | PRD 原方案 | 要重买手柄，且 HID 描述符差异同样是坑 | 仅在 A/B/C 全失败时考虑 |

**本方案的最大风险已从"协议未知"降级为"移植工作量"** —— 因为协议已经完整拿到（第 2 章）。
这跟 PRD R-01 里担心的"不同手柄 HID 描述符差异大、必须实测解析"**已经不是同一个问题了**。

**仍需实测确认的未知项**：
- 摇杆物理方向与符号（哪个方向为正）
- 实际的 notify 帧率（决定指向环带宽）
- 双手柄/多设备环境下的干扰情况

---

## 9. 已同步到 PRD 的修订（PRD v1.3）

本文件确认了 S10 不是 BLE-HID 后，PRD 已在 **v1.3** 同步修订，以下改动**均已落地**
（历史对照见 PRD 修订记录表）：

| PRD 位置 | v1.2 的写法 | v1.3 改为 |
|---|---|---|
| §5.4 | "ESP32 作为 BLE 主机（HID Host）… 使用 `esp_hid_host` 组件" | **整节重写**：NimBLE GATT 客户端，服务 `0xFFA0` / 特征 `0xFFA1`，含帧格式与两个手柄硬件坑 |
| §2 FR-06 / SR-01 / SR-05 | "连接标准 HID 手柄 / 未收到 HID 报告 / 手柄急停键" | 改为 notify 帧语义；急停改为 **L1+R1+Start 组合键**（Home 禁用） |
| §4.1.2 | `gamepad_ble/` 依赖 `esp_hid_host` | 依赖 NimBLE；新增 `codexpad_codec.c/.h`（帧编解码） |
| §4 分层 / 任务 / sdkconfig | "BLE HID" / "BLE HID host" | BLE(NimBLE) |
| §3.1 硬件清单 | 未列手柄具体型号 | 补 CodexPad-S10 规格（BLE 5.3 从机、17 键、双摇杆 8 位、400 mAh） |
| §6.3 | 用 `esp_hid_host` 论证 ESP-IDF 的合理性 | 改为"实时确定性"论证；承认 Arduino/MicroPython 也能做 BLE 主机 |
| §7 M2 | "`esp_hid_host` 示例改为连手柄" | 改为 NimBLE `blecent` 裁剪 + M2a/M2b/M2c 分阶段验收 |
| R-01 | "不同手柄 HID 描述符差异大"（概率高） | 降级为"移植工作量"（概率中），协议已知 |
| Q-10 | 手柄型号待确认 | **已确认：CodexPad-S10**，剩余摇杆方向/符号、实际帧率待实测 |
| 术语表 / 参考索引 | BLE HID Host / HID report | 改为 BLE central / NimBLE / GATT / notify / CodexPad-S10 / CRC-8/SAE-J1850 |

> 此外 README、`firmware/README.md`、`hardware/README.md` 里的对应描述也已一并更新。

---

## 附：官方资源索引

| 资源 | 地址 | 用途 |
|---|---|---|
| S10 手册（GitHub 镜像） | `github.com/CodexPad/codex_pad_s10` | 产品规格、指示灯、BD_ADDR、开关机 |
| 连接指南（原生 BLE） | `github.com/CodexPad/codex_pad_guide` → `connection_guide_native_ble.zh-CN.md` | 两种连接方式说明 |
| Arduino 库（连接） | `github.com/CodexPad/codex_pad_arduino_lib` | GATT UUID、连接流程的权威对照 |
| 帧编解码 | `github.com/CodexPad/gamepad_codec_arduino_lib` | 载荷格式 |
| 帧框架 + CRC | `github.com/CodexPad/robust_frame_arduino_lib` | 0xAA/0x55/转义/CRC8 |
| 输入类型定义 | `github.com/CodexPad/gamepad_input_arduino_lib` | `State` / `Button` / `Axis` |
| PC 端 Python 库 | `github.com/CodexPad/codex_pad_bleak_lib` | 诊断对照 |
| MicroPython 库 | `github.com/CodexPad/codex_pad_mpy_lib` | 备选路线 B |
| 原始手册（Gitee） | `gitee.com/CodexPad/codex_pad_s10` | 用户提供的原始链接 |
