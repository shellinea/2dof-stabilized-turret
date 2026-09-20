# 03 - BLE 手柄接入执行文档

**里程碑 M2：打通「CodexPad-S10 手柄 → ESP32-S3」的蓝牙输入链路**

> **这是一份"照着做"的手册，不是设计文档。**
> 每一步都给出：**要做什么 → 敲什么命令 → 看到什么算成功 → 没成功怎么办**。
> 协议细节（notify 载荷格式、GATT 全表、键位表）不用现在读，全在**附录 A**，写到对应代码时再翻。
>
> 只用手柄 + 板子 + 两根 USB 线。**不需要** CAN 收发器、电机、IMU、泰山派。

| 项 | 内容 |
|---|---|
| 版本 | v2.4（2026-09-19，M2c 方案 1 + 满量程归一化修正） |
| 关联 | `docs/00-总体方案-PRD.md` §5.4、§4.1.2、§4.1.3、里程碑 M2 |
| 预计总工时 | 环境 0.5~1 天（大半在下载），代码 2~3 天 |

---

## 进度勾选表

做完一格勾一格。**建议严格按顺序**，每格跑通再进下一格。

| 步骤 | 内容 | 预计 | 完成 |
|---|---|---|---|
| **1** | 装 ESP-IDF + VS Code + CH340 驱动，确认串口号 | 0.5~1 天 | ✅ ESP-IDF 已装（2026-09-18） |
| **2** | 用 PC 先验证手柄（二分法基准） | 0.5 h | ⬜ |
| **3** | 建工程骨架，空跑一次烧录 | 0.5 h | ✅ 2026-09-18 |
| **4** | **M2a** 只扫描，不连接 | 半天 | ✅ 2026-09-19（17 键位图全通） |
| **5** | **M2b** 连接 + 订阅 + 解析 | 1~2 天 | ✅ 2026-09-19（AC-BLE-2..6 全过） |
| **6** | **M2c** 映射为 `ω_ref` | 半天 | ✅ 2026-09-19（AC-BLE-7..10 全过，AC-BLE-11 待验） |
| **7** | **M2d** 接 CAN（需先买收发器） | — | ⬜ |

---

# 0. 先回答三个问题（第一步到底做什么）

你问的"是要下载某个固件，还是先配好环境"，答案很明确：

| 问题 | 答案 |
|---|---|
| 要不要先刷一个现成固件进去？ | **不要**。板子出厂固件不用动，也**不需要刷 MicroPython**。我们从零写 C 代码，编译后用 ESP-IDF 烧进去。 |
| 要不要先配 VS Code？ | **已经配好了**。扩展装了、也指向了本机安装（见 §1.2），不用你手工配。 |
| 第一步到底干什么？ | ~~装 ESP-IDF~~ → **已经装完了**（2026-09-18，见 §1.0）。所以第一步实际是：**双击桌面 `IDF_v5.5.5_Powershell` 验证 `idf.py --version` 能打印版本号**，然后直接进第 3 步建工程。 |

**整体顺序（依赖倒推出来的，不要跳）**：

```
第1步 装 ESP-IDF  ──►  第3步 建工程  ──►  第4步 M2a 扫描
      （环境）          （骨架）           （证伪协议理解）
                                    │
第2步 PC 验证手柄 ──────────────────┘   ← 并行做，用来二分"是板子的问题还是手柄的问题"
                                    │
                                    第5步 M2b 连接+解析  ──►  第6步 M2c 映射 ω_ref
```

**为什么第 2 步要在写板子代码前先做**：如果第 4 步扫不到手柄，你没法判断是
"手柄坏了"、"手柄被别人连走了"还是"我 NimBLE 代码写错了"。先用 PC 读通，
第 4 步出问题时就能一刀切开，**这是最省时间的做法**。

---

# 1. 装 ESP-IDF（Windows）—— ✅ 已完成（2026-09-18）

> 本步产出：`idf.py` 能跑；VS Code 扩展已指向本机安装；设备管理器里能看到板子的串口号。

### 1.0 本机实际装法（**已装完，不用重做，留作记录**）

用的是乐鑫官方的 **EIM 命令行安装器**（`eim-cli` v0.19.0），装的是
**ESP-IDF v5.5.5**，**全部放在 D 盘**：

| 项 | 实际路径 |
|---|---|
| ESP-IDF 源码 | `D:\esp\v5.5.5\esp-idf` |
| 工具链 / 编译器 / Python 环境 | `D:\esp\tools` |
| 下载缓存（1.6 GB，删了也能用） | `D:\esp\dist` |
| 安装记录（**VS Code 扩展读的就是它**） | `D:\esp\eim_idf.json` |
| 环境激活脚本 | `D:\esp\tools\Microsoft.v5.5.5.PowerShell_profile.ps1` |
| **桌面快捷方式** | `IDF_v5.5.5_Powershell.lnk` |

**为什么放 D 盘不装默认的 `C:\Espressif`**：C 盘当时只剩 11 GB，而工具链
（编译器 + CMake + Ninja + OpenOCD + Python 环境）要占 **7~8 GB**，会把系统盘挤爆。

**已验证可用**：`idf.py --version` → `ESP-IDF v5.5.5`；
`examples/get-started/hello_world` 对 `esp32s3` **编译通过**（放在
`D:\esp\buildtest\hello_world`，想先烧个现成的试试板子可以直接用它）。

### 1.1 每次开发：先激活环境（本节是最容易忘的概念）

**ESP-IDF 不是一个"装在系统里的软件"，而是一堆环境变量**：
`IDF_PATH`、`IDF_TOOLS_PATH`、以及编译器 / CMake / Ninja 的路径。
**不激活，`idf.py` 这个命令根本不存在。** 所以每次开新终端都要先激活。

**方式 A（推荐）**：双击桌面的 **`IDF_v5.5.5_Powershell`** ——
它开出来的 PowerShell 里环境已经配好，直接敲 `idf.py` 即可。

**方式 B（手动激活，任何 PowerShell 里都行）**：

```powershell
. D:\esp\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
```

**方式 C（VS Code）**：装了 ESP-IDF 扩展后，VS Code 集成终端里有一排
"ESP-IDF Terminal"，效果相同。

**验证激活成功**：

```powershell
idf.py --version
```

**成功标志**：打印 `ESP-IDF v5.5.5`。
**失败**：提示 `无法将"idf.py"项识别为...` → 环境没激活，回到方式 A。

> ⚠ **别在 Git Bash / MSYS2 里跑 `idf.py`**：ESP-IDF v5.5 会直接拒绝并打印
> `MSys/Mingw is no longer supported`，什么都不做。用 PowerShell 或 CMD。

### 1.2 VS Code 扩展（已配好，不用再动）

扩展 `espressif.esp-idf-extension` **已经装好**（本机是 **v2.2.0**），
并且已在 `settings.json` 里指向本机安装：

```json
"idf.eimIdfJsonPath": "D:\\esp\\eim_idf.json",
"idf.currentSetup": "D:\\esp\\v5.5.5\\esp-idf"
```

**为什么要这两行**：扩展 v2.2.0 默认只去 `C:\Espressif\tools\eim_idf.json`
找安装，而我们把东西装在 D 盘，不指路它根本找不到 → 界面一片空白。

> ⚠ **网上教程里的 `ESP-IDF: Configure ESP-IDF Extension` 命令在 v2.2.0 里已经不存在了**
> （v2.x 改成围绕 EIM 的配置模型）。之前输入它"没有任何显示"，就是这个原因。
> 要检查或切换版本，用命令面板的 **`ESP-IDF: 选择当前使用的 ESP-IDF 版本`**，
> 或 **`ESP-IDF: Open Get Started Walkthrough`**。

用法：VS Code → **文件 → 打开文件夹** → 选工程目录（如 `D:\esp\turret_ble`）。
底部状态栏会出现一排图标：

- 🔌 **选 COM 口**（烧录/看日志用）
- 🎯 **选芯片**（点一下，选 `esp32s3`）
- 🔥 **火焰图标 = 编译+烧录+看串口**
- 📺 **插头图标 = 只看串口日志**

> **不想用 VS Code 完全没问题** —— 本手册所有命令都在激活好的终端里敲，
> 效果一模一样。VS Code 只是图形化包装。**先用命令行跑通，再考虑要不要换 VS Code。**

### 1.3 ⚠ 工程放哪：路径必须纯英文，不要有空格

**别嫌啰嗦，踩了会浪费一晚上。** ESP-IDF 底层是 CMake + Ninja + GCC，
这套工具链对**非 ASCII 路径**（中文、空格）会偶发报莫名其妙的错：
找不到文件、路径被截断、`ninja: error: manifest ...`。

**不要**把工程建在：

- ❌ `D:\用户\桌面\步进电机项目\`（有中文）
- ❌ `C:\Users\...\Desktop\新建文件夹\`（有中文）

**建在**：

- ✅ `D:\esp\`（纯英文、无空格）

> 本手册后面所有命令都以 `D:\esp\turret_ble\` 为例。工作台文件夹继续放文档，
> **代码另起一个纯英文目录**。这不影响你最后把成果整理进 GitHub 仓库。

### 1.4 装 CH340 驱动（板子插上后设备管理器没串口才需要）

**本板板载的 USB 转串口芯片是 CH340（不是 CP210x）**，所以要装 CH340 驱动。

1. 用 USB 线把板子插上电脑（插板上**标着 `UART`/`COM` 的那个 USB-C 口**，不是 `USB` 口）
2. 右键"开始" → **设备管理器** → 展开 **端口 (COM 和 LPT)**
3. 看两种情况：

| 看到的 | 说明 | 处理 |
|---|---|---|
| `USB-SERIAL CH340 (COM5)` 之类 | ✅ 驱动已就绪 | **记下这个 COM 号**（如 `COM5`），后面全用它 |
| 出现 `CH340` 带黄色感叹号，或出现在"其他设备"下 | 缺驱动 | 去设备官网下载 **CH340 驱动**（WCH 官网搜 `CH341SER`）装上，重插 |
| 什么都没有 | 线插错口了 / 线是只能充电的 | 换板子上另一个 USB-C 口试试；换一根能传数据的线 |

> ⚠ **线材坑**：有些 USB 线只有电源线没有数据线，插上只亮电源灯但不出 COM 口。
> 换一根确定能传数据的线。

**本步成功标志**：设备管理器里能看到 `USB-SERIAL CH340 (COMx)`，且记下了 `COMx`。

---

# 2. 先用 PC 验证手柄（强烈建议，半小时）

> **目的**：拿到一个"手柄本身是好的、协议我理解对了"的基准。
> 第 4 步在板子上扫不到手柄时，这个基准能立刻告诉你"不是手柄的锅"。

### 2.1 装 `bleak`（PC 端 Python BLE 库）

**本机 pip 直连 pypi.org 会 TLS 断连**，必须加清华镜像：

```powershell
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple bleak
```

### 2.2 拿官方的 PC 端库

从厂商开源仓库拿 **`codex_pad_bleak_lib`**：

```powershell
cd D:\esp
git clone https://github.com/CodexPad/codex_pad_bleak_lib.git
cd codex_pad_bleak_lib
```

> **网络提示**：本机实测 `curl api.github.com` 能通，但 `raw.githubusercontent.com` 解析不了。
> 如果 `git clone` 也失败，改用浏览器打开
> `https://github.com/CodexPad/codex_pad_bleak_lib`，点 **Code → Download ZIP**。

### 2.3 ⚠ 手柄开机时序（这个规则贯穿整个项目）

**必须先记住，否则每一步都会被它坑：**

1. **短按中央 Home 键**开机
2. 1 号蓝灯**慢闪（约 1 秒亮/灭）= 正在广播，可以连**
3. ⚠⚠ **广播状态下超过 1 分钟没被连接，手柄自动关机** ← 开发时最容易踩的坑
4. 连上之后 1 号蓝灯**常亮**

**所以每次做扫描测试，都是这个节奏**：先让脚本开始扫描 → 再短按 Home 开机 →
在 1 分钟内扫到。**反过来（先开机再折腾脚本）大概率手柄已经自己关了。**

### 2.4 跑扫描，确认能读到手柄

```powershell
python scan.py        # 文件名以仓库里的 README 为准
```

**成功标志**：列表里出现 `CodexPad-XXXX`，带 BD_ADDR、RSSI，且**按下不同按键时
扫描响应里的按键状态跟着变**（⚠ 是**扫描响应**，不是主广播——见 §A.5）。

**若扫不到**（按这个顺序排查）：

1. 手柄是不是已经过了 1 分钟自己关了？→ 重开
2. 电脑蓝牙开了吗？→ 设置 → 蓝牙，确认打开
3. 手柄被手机/别的电脑连走了吗？→ 把附近已配对的设备关掉蓝牙
4. 手柄电量低？→ 充电

> **本步产出**：一条**BD_ADDR**（手柄背面标签上也印着，格式 `XX:XX:XX:XX:XX:XX`），
> 第 5 步连接时要用。**记下来。**

---

# 3. 建工程骨架

> **目的**：得到一个能编译、能烧录、能出日志的空工程。
> **先把"工具链能跑"和"代码写得对"分开验证** —— 别一上来就写 BLE 代码。

### 3.1 从官方示例复制，不要从零写

ESP-IDF 自带的 **`blecent`**（NimBLE central 示例）已经包含了
BLE 主机初始化、地址获取、事件循环的全部样板。**照搬它，只改我们特有的部分。**

```powershell
mkdir D:\esp
cd D:\esp

# 从 ESP-IDF 安装目录复制示例
Copy-Item -Recurse "$env:IDF_PATH\examples\bluetooth\nimble\blecent" .\turret_ble
cd turret_ble
```

> `$env:IDF_PATH` 是激活环境时自动设好的变量，指向安装目录。
> 敲 `echo $env:IDF_PATH` 应该打印出 **`D:\esp\v5.5.5\esp-idf`**（本机的实际路径）。

### 3.2 设目标芯片

```powershell
idf.py set-target esp32s3
```

### 3.3 配置：16 MB Flash + OPI PSRAM（照抄，别改）

```powershell
idf.py menuconfig
```

在菜单里改这几项（用方向键 + 回车，`Q` 保存退出）：

| 菜单路径 | 改成 | 为什么 |
|---|---|---|
| `Serial flasher config` → `Flash size` | **16 MB** | 不改成 16 MB 会发现"代码放不下" |
| `Component config` → `ESP PSRAM` → `Support for external, SPI-connected RAM` | **勾上** | 启用 PSRAM |
| `Component config` → `ESP PSRAM` → `SPI RAM config` → `Mode (QUAD/OCT)` | **Octal Mode PSRAM** | N16R8 的 PSRAM 是**八线**的。**不选 OCT，PSRAM 用不了** |
| `Component config` → `Bluetooth` → `Bluetooth` | **Enabled** | BLE 总开关 |
| `Component config` → `Bluetooth` → `Host` | **NimBLE - NimBLE Stack** | 用 NimBLE，不是 Bluedroid |

> **也可以直接改文件**：在工程根目录新建/编辑 `sdkconfig.defaults`，写：

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
```

### 3.4 空跑一次：编译 + 烧录 + 看日志

```powershell
idf.py -p COM5 flash monitor
```

（`COM5` 换成第 1.5 步记下的串口号）

**成功标志**：

- 编译到 100%，输出 `Project build complete`
- 烧录进度到 100%，`Hash of data verified`
- 串口开始打印日志，能看到 `I (xxx) main_task: ...` 一类的行
- **退出监视器**：按 `Ctrl + ]`

**失败**：

| 现象 | 原因 | 处理 |
|---|---|---|
| `Failed to connect to ESP32-S3: No serial data received` | 板子没进下载模式 | 按住 `BOOT` 键 → 点一下 `RST` 键 → 松开 `BOOT`，再重跑命令 |
| `could not open port COM5` | COM 口被别的程序占用 | 关掉 Arduino IDE / 串口助手 / 上一个 monitor 窗口 |
| 编译报错找不到 target | 环境没加载 | 确认环境已激活（§1.1），且 `idf.py set-target esp32s3` 跑过 |
| 串口乱码 | 波特率不对 / 板子复位中 | IDF monitor 自己会处理；若用别的工具看，用 115200 |

> **本步产出**：`idf.py flash monitor` 这条命令能一条龙跑通。
> **这一步过了，后面所有"是代码问题还是环境问题"的疑惑都少一半。**

---

# 4. M2a —— 只扫描，不连接

> **目的**：用最少的代码证明"板子蓝牙正常 + 手柄在广播 + 我们理解对了协议"。
> **连都不用连**，所以失败原因极其有限，是最好调的一步。

### 4.1 要做的事

改 `main/` 里的扫描部分：

1. **扫描参数必须是主动扫描**：`disc_params.passive = 0`、`disc_params.filter_duplicates = 0`
   （`blecent` 原版写的是 `passive = 1`，照抄会拿不到厂商数据，见 §4.2 末尾）
2. **按名字认出设备后记住它的 BD_ADDR**，之后**按地址**匹配（⚠ 关键，见下）
3. 打印：**设备名、BD_ADDR、RSSI**
4. 解析**扫描响应**里的 **Manufacturer Specific Data**，打印固件版本与**按键状态**（翻成按键名）

> ⚠ **为什么必须按地址认、不能只按名字认**
>
> 手柄的按键数据在 **SCAN_RSP** 里，而 **SCAN_RSP 里没有 name 字段**
> （ADV_IND 才有名字）。只按名字过滤的话，所有带按键数据的包会被全部丢掉，
> 表现就是"扫描看着正常、但永远读不到按键"。
>
> **实测踩过的坑**：第一版按名字认，62 秒收到 1566 帧、全是没按键的 ADV_IND，
> 于是误判成"这只手柄不回扫描请求、文档写错了"，白白多烧了两轮固件。
> 正确做法：名字 → 记住地址 → 之后地址匹配。

### 4.2 关键代码：扫描与解析扫描响应

`blecent` 示例里已经有 `ble_gap_disc()` 和 `gap_event` 回调，找到 `BLE_GAP_EVENT_DISC`
分支，改成下面这样：

```c
#include "host/ble_hs_adv.h"

/* 按键位表（完整 17 键，见附录 A.4） */
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
    for (size_t i = 0; i < sizeof(k_btn_names)/sizeof(k_btn_names[0]); ++i)
        if (m & k_btn_names[i].bit) printf(" %s", k_btn_names[i].name);
    printf(" ]\n");
}

/* 手柄地址：第一次靠名字认出来之后记住它。
 * 之后连"没有 name 字段的扫描响应"也能认出来 —— 只按名字认会漏掉那类包。 */
static ble_addr_t g_pad_addr;
static int        g_pad_addr_ok;

static int is_pad_addr(const ble_addr_t *a)
{
    return g_pad_addr_ok && a->type == g_pad_addr.type &&
           memcmp(a->val, g_pad_addr.val, 6) == 0;
}

static void on_disc(const struct ble_hs_adv_fields *f,
                    const struct ble_gap_disc_desc *disc)
{
    int named = (f->name != NULL && f->name_len >= 8 &&
                 memcmp(f->name, "CodexPad", 8) == 0);

    if (named && !g_pad_addr_ok) {
        g_pad_addr = disc->addr;
        g_pad_addr_ok = 1;
        printf("\n[认出手柄地址] addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
               disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
               disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
    }

    /* ⚠ 这里是关键：named 为假时还要再按地址认一次 */
    if (!named && !is_pad_addr(&disc->addr)) return;

    printf(">>> %s evt=0x%02X  RSSI=%d dBm  addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
           named ? "发现手柄" : "手柄(无名字包)", disc->event_type, disc->rssi,
           disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
           disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);

    /* Manufacturer Specific Data（结构见附录 A.5） */
    const uint8_t *m = f->mfg_data;
    if (m && f->mfg_data_len >= 2 + 8 + 3 + 4 + 1) {
        if (m[0] == 0xFF && m[1] == 0xFF &&
            memcmp(&m[2], "CodexPad", 8) == 0) {
            uint8_t  vmaj = m[10], vmin = m[11], vpat = m[12];
            uint32_t btn  = (uint32_t)m[13] | ((uint32_t)m[14] << 8) |
                            ((uint32_t)m[15] << 16) | ((uint32_t)m[16] << 24);
            printf("    fw=%u.%u.%u  held=%us\n", vmaj, vmin, vpat, m[17]);
            print_buttons(btn);
        }
    }
}
```

在 `gap_event` 回调的 `BLE_GAP_EVENT_DISC` 分支里，把 `ble_hs_adv_parse_fields()`
的结果喂进去（**不要**在这一步发起连接）：

```c
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) == 0) {
            on_disc(&fields, &event->disc);
        }
        return 0;
    }
```

**照抄 `blecent` 会踩的两个坑**（`blecent_scan()` 里，`disc_params` 初始化之后
`blecent` 又显式改了这两个字段）：

```c
    /* ⚠ 必须为 0：手柄靠"重复广播"持续上报按键状态，
     * 过滤掉重复就只看得到第一帧，按什么键都不会变了。*/
    disc_params.filter_duplicates = 0;

    /* ⚠ 必须为 0（主动扫描）：只被动扫描不会发 SCAN_REQ，就拿不到 SCAN_RSP，
     * 而按键数据在扫描响应里。blecent 原版这里写的是 1。*/
    disc_params.passive = 0;
```

扫描参数（让 BLE 主机持续扫描，不过滤重复）：

```c
struct ble_gap_disc_params dp = {
    .itvl            = 0,        /* 0 = 用默认（快） */
    .window          = 0,
    .filter_policy   = 0,
    .limited         = 0,
    .passive         = 0,        /* 主动扫描，才能拿到 scan response 里的名字 */
    .filter_duplicates = 0,      /* ⚠ 必须为 0，否则同一设备只报一次 */
};
ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_event, NULL);
```

### 4.3 烧录并观察

```powershell
idf.py -p COM5 flash monitor
```

> ⚠ **记住动手时序**：**先在终端里敲完命令、看到 monitor 启动了、屏幕上开始滚日志**，
> **再短按手柄 Home 开机**。然后你有 1 分钟时间看结果。

> ⚠ **不要用"录 N 秒"这种固定窗口**。手柄开机后 1 分钟无连接就自动关机，
> 固定窗口极易撞上"窗口开了、手柄还没开"或"手柄刚关、窗口还没到"，
> 白白多烧好几轮固件。**开一个不限时的捕获，按自己的节奏操作，按完了再停掉看日志。**

### 4.4 验收（M2a）

| 编号 | 验收项 | 判定 |
|---|---|---|
| AC-BLE-1a | 串口打印出 `CodexPad-XXXX`，带 RSSI 与 BD_ADDR | ✅ |
| AC-BLE-1b | 打印出 `fw=x.y.z` 固件版本 | ✅ |
| AC-BLE-1c | **按下不同按键，`buttons=` 那行的方括号里跟着变** | ✅ |

**2026-09-19 实测结果**：三条全过。硬件 `YD-ESP32-S3-N16R8`，手柄 `addr=16:00:00:00:19:12`、
`fw=2.3.2`，17 键位表逐一按下全部对上（含 R3 与 L1+R1 组合键）。
记录两个实测数字供后续设计参考：**按键更新率 5~19 Hz 且随开机时间衰减**，
**扫描响应里没有摇杆模拟量** —— 所以 M2b 是绕不过去的。

> **这一步跑通，R-01 的大半不确定性就消掉了** —— "能扫到、能读到按键位"
> 说明协议情报无误，剩下的只是体力活。
>
> **如果这里就卡住了，别硬调**：回头确认第 2 步 PC 上能不能读到。
> PC 能读 → 是板子代码问题；PC 也读不到 → 是手柄/环境问题，跟 ESP-IDF 无关。

---

# 5. M2b —— 连接 + 订阅 + 解析

> **目的**：拿到连续的摇杆数据。
> 这一步分两小步：**先在 PC 上把帧解析器单测通，再上板**。

### 5.1 【先做这个】在 PC 上把 notify 的真实格式钉死

**为什么**：M2b 的调试时间几乎全花在"以为 notify 是某种带帧格式，其实不是"上。
先在 PC 上读十几包**原始 hex**，格式当场就定下来了，上板后若出错就只剩 BLE 的问题。

```python
# dump_notify.py —— 只打印原始字节，**不做任何解析**（解析是"待验证的假设"，
# 一旦先写了解析器，就会不自觉地拿它去套，看不到真相）
import asyncio
from bleak import BleakClient

ADDR = "XX:XX:XX:XX:XX:XX"                     # §2.4 记下的 BD_ADDR
UUID_INPUT = "0000ffa1-0000-1000-8000-00805f9b34fb"

def on_rx(_, data: bytearray):
    print(len(data), data.hex(" "))

async def main():
    async with BleakClient(ADDR) as c:
        await c.start_notify(UUID_INPUT, on_rx)
        for _ in range(15):
            await asyncio.sleep(1)

asyncio.run(main())
```

**⚠ 时序**：**先让脚本跑起来**（它会阻塞等连接），**再短按 Home 开机** ——
手柄只在广播 1 分钟（§2.3），反过来做大概率它已经自己关了。

**成功标志（2026-09-19 实测）**：打印出来是**定长 8 字节**，且**没有帧头帧尾**：

```
8  00 00 00 00 80 80 80 80      ← 静置
8  00 00 00 00 5e 80 80 80      ← 推左摇杆，第 5 字节在变
```

**这就是结论**：附录 A.3 那套 `0xAA…0x55` / 转义 / CRC8 的 `robust_frame` 封装
**在这只机器上根本不出现**。载荷是裸的 8 字节，直接 `memcpy` 就能用，
**不需要写编解码器**（字段定义见 A.3）。

> 早期版本这里让人先写一个 CRC8 编解码器单测（`tools/frame_test.py`）。
> 那份代码留着当 CRC8 的记录也行（它确实和标准向量 `crc8("123456789") == 0x4B` 对上了），
> 但**本项目用不到** —— 没有帧，就没有 CRC 要校验。

### 5.2 上板：连接手柄

在 `blecent` 示例的事件回调里，扫描到目标后发起连接：

```c
/* 5.2-1  扫描到目标（可由按键掩码选中，见附录 A.5）后发起连接 */
ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 10000, NULL,
                gap_event, NULL);
```

```c
/* 5.2-2  连接成功 → 发现服务 0xFFA0 → 发现特征 0xFFA1 */
if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
    conn_handle = event->connect.conn_handle;
    ble_uuid16_t svc = BLE_UUID16_INIT(0xFFA0);
    ble_gattc_disc_svc_by_uuid(conn_handle, &svc.u, on_svc_disc, NULL);
    return 0;
}
```

> **想一次看全表**就换成 `ble_gattc_disc_all_svcs()` 再把每个服务的特征都翻一遍
> —— 实测这样才发现了文档没写的第二个服务 **`0xFFE0`**（见 A.6）。
> 只要 M2b 的数据，定向找 `0xFFA0` 就够了。

```c
/* 5.2-3  找到特征后，往下找它的 CCCD（UUID 0x2902），写 {0x01,0x00} 开 notify */
static const ble_uuid16_t k_cccd = BLE_UUID16_INIT(0x2902);
/* on_chr_disc 里存下 chr_handle，再 ble_gattc_disc_all_dscs(...) 找 CCCD；
   找到后： */
uint8_t on[2] = { 0x01, 0x00 };
ble_gattc_write_flat(conn_handle, cccd_handle, on, sizeof(on), NULL, NULL);
```

```c
/* 5.2-4  收到通知 → 直接按 8 字节裸状态取值 */
if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
    struct os_mbuf *om = event->notify_rx.om;
    uint8_t buf[64];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len > sizeof(buf)) len = sizeof(buf);
    os_mbuf_copydata(om, 0, len, buf);
    handle_notify(buf, len);                     /* 见 5.3 */
    return 0;
}
```

### 5.3 载荷解析（**不是**状态机 —— 没有帧要组）

载荷是**定长 8 字节**，`memcpy` 进结构体即可，没有帧头/转义/CRC 需要处理：

```c
typedef struct __attribute__((packed)) {
    uint32_t buttons;      /* 小端 */
    uint8_t  axes[4];      /* Lx, Ly, Rx, Ry；中心 0x80（字段定义见 A.3/A.4） */
} codexpad_state_t;        /* sizeof == 8 */

static void handle_notify(const uint8_t *buf, uint16_t len)
{
    if (len != sizeof(codexpad_state_t)) {       /* 长度不符 → 丢弃并报一次 */
        printf("[notify] ⚠ 长度 %u ≠ 8，丢弃\n", len);
        return;
    }
    codexpad_state_t st;
    memcpy(&st, buf, sizeof(st));

    /* 覆盖式队列：永远用最新样本，不积压 */
    xQueueOverwrite(s_gamepad_q, &st);
}
```

⚠⚠ **这一节最重要的不是解析，是"什么时候没有包"**：

**notify 是变化驱动的（change-driven），不是周期流。** 实测（2026-09-19）：
手柄只在**状态真的变了**的时候才发一包，动的时候约 **32 Hz**，
**手一停一包都不发**。

对控制环的硬约束：

| 现象 | 错误做法 | 正确做法 |
|---|---|---|
| 手不动 → 没有 notify | 当成"手柄掉线"，超时清零 / 停转 | **保持上一次的值**，没有新包 = 状态不变 |
| 手柄真的掉线 | 分不清 | 靠 `BLE_GAP_EVENT_DISCONNECT` 判断，**不能靠"多久没收到包"** |

**所以看门狗不能挂在 notify 上**，要挂在 GAP 层的连接事件上。
这一点在 M2c（映射成 `ω_ref`）时会直接影响设计 —— 误停比不响应危险得多。

### 5.4 验收（M2b）

| 编号 | 验收项 | 判定 |
|---|---|---|
| AC-BLE-2 | 连接成功，**手柄 1 号蓝灯常亮** | ✅ 连接耗时约 260 ms，GATT 全表 10 次发现全部 rc=0 |
| AC-BLE-3 | 推任意摇杆，串口连续打印 `Lx/Ly/Rx/Ry`；范围大致覆盖 `0~255`；**松手回 `0x80` 附近** | ✅ 四轴全通；回中 = `0x80`；满量程踩到 `0` 和 `255`；上/前 = 数值增大 |
| AC-BLE-4 | **17 个按键逐个按**：扫描响应按键位（M2a 的打印）与 notify 解析结果**一致**，无串扰 | ✅ 17 键（含 L3/R3/Home）位图与 M2a **完全一致**，无一位不同 |
| AC-BLE-5 | 手柄关机再开机 → ESP32 **能自动重连**（`BLE_GAP_EVENT_DISCONNECT` 里重新发起扫描） | ✅ 中途断了一次（`reason=520` 连接超时），约 360 ms 内自动重连 + 重新订阅，notify 无缝续上 |
| AC-BLE-6 | **连续跑 10 分钟**：不丢帧、不重启；**记下每秒多少完整帧**（这是指向环的输入频率上限） | ✅ 跑满约 3.5 分钟无丢帧/重启。**速率见下** |

**AC-BLE-6 实测速率（2026-09-19）**：串口打印的 `累计 N 包 / T ms` 是**被空闲段稀释过的平均值**
（`1.0 Hz → 1.9 Hz`，因为大部分时间手没动）。真实瞬时速率看两次采样的**差值**：

```
200 包 / 6262 ms ≈ 32 Hz      ← 持续推摇杆时
```

静置时 **0 Hz**（448 个包里没有一个是重复状态）。所以：

- **输入频率上限 ≈ 32 Hz**（动的时候），指向环按 30~50 Hz 设计是合理的；
- **这是变化驱动，不是周期流** —— 控制环必须容忍"长时间收不到包"，把"没有新包"
  当成"状态不变"而不是"掉线"（见 5.3 的警告）。


---

# 6. M2c —— 映射为 `ω_ref`

> **目的**：把摇杆数值变成"期望角速度"，为第 7 步接电机做准备。
> **本步只打印数值，不下发任何东西。**

### 6.1 ⚠ 先解决"包不来"的问题（方案 1：保持上一次的值）

**这一节是本步最重要的一节，先看它，再看归一化。**

M2b 实测（§5.3/§5.4）：手柄的 notify 是**变化驱动**，手一停**一包都不发**。
所以控制环**必然**会遇到"几十秒收不到任何包"的正常情况。此时有两种做法：

| 方案 | 做法 | 后果 |
|---|---|---|
| ❌ 超时归零 | 超过 N ms 没收到包 → 认定失效，`ω_ref = 0` | **摇杆停在中间位置不动，云台就自己停了**；把"没消息"误当"坏消息" |
| ✅ **方案 1：保持上一次的值** | 没收到包就用**队列里存的最后一个状态**，永不"过期" | 摇杆不动 = 云台保持当前角速度，物理上正确 |

**实现：长度 1 的覆盖式队列（`xQueueOverwrite` / `xQueuePeek`）**

```c
static QueueHandle_t g_pad_q;        /* codexpad_state_t × 1，覆盖式，单写单读 */

/* 收到一包就覆盖进去；没收到包时队列里还是上一次的值 —— 这就是方案 1 的全部实现 */
static void push_state(const codexpad_state_t *st) { xQueueOverwrite(g_pad_q, st); }

/* 真正的失效处理：只在 GAP 断开时调用 */
static void push_center(void) {
    codexpad_state_t z;
    memset(&z, 0, sizeof(z));
    z.axes[0] = z.axes[1] = z.axes[2] = z.axes[3] = 0x80;
    push_state(&z);
}
```

**失效判据必须挂在 GAP 连接事件上，不能挂在数据流上**：

```
BLE_GAP_EVENT_NOTIFY_RX  → push_state(新状态)        ← 数据来了就更新
BLE_GAP_EVENT_DISCONNECT → push_center()             ← 唯一的清零点（AC-BLE-8）
```

> **反面教材**：把看门狗挂在 notify 上。那样"摇杆推着不动"和"手柄关机了"
> 这两种情况**在代码里长得一模一样**，必然误停。误停（云台突然不响应）
> 比不响应危险得多。

### 6.2 归一化 + 死区

```c
/* 轴值 0..255，中心 0x80 → 归一化 [-1.0, +1.0]
 * ⚠ 分母必须**按方向取**：中心到 0 是 128 个计数，中心到 255 是 127 个。
 *   统一用 127 会让负半轴超到 -1.008（满量程打出 -60.47 而不是 -60.00），
 *   现在只是打印难看，接上电机就是反向 0.8% 过冲。
 *   2026-09-19 实测踩到：就是这条式子写错才出现的 -60.47。 */
static inline float axis_norm(uint8_t raw) {
    float v = (float)raw - 128.0f;
    return v / (v < 0.0f ? 128.0f : 127.0f);
}

/* 死区：避免摇杆回中不准导致的漂移 */
#define AXIS_DEADZONE 0.08f
static inline float apply_deadzone(float v) {
    if (v > -AXIS_DEADZONE && v < AXIS_DEADZONE) return 0.0f;
    return v;
}

#define OMEGA_MAX_DPS 60.0f     /* 摇杆推满对应的角速度，先取 60 °/s，实测再调 */

float omega_pan  = apply_deadzone(axis_norm(st.axes[0])) * OMEGA_MAX_DPS;  /* 左摇杆 X */
float omega_tilt = apply_deadzone(axis_norm(st.axes[3])) * OMEGA_MAX_DPS;  /* 右摇杆 Y */
```

> `axes[]` 顺序固定是 **Lx, Ly, Rx, Ry**（索引 0/1/2/3），
> **实测两杆都是"上/前 = 数值增大"**（§A.4）。
> 但**符号与转台实际正方向的对齐**要等 M2d 接上电机才能定 ——
> 届时若方向反了，把 `OMEGA_MAX_DPS` 取负即可，不用改结构。

**控制环结构**（50 Hz 跑，只在数值变化时打印）：

```c
static void m2c_task(void *arg)
{
    while (1) {
        codexpad_state_t st;
        if (xQueuePeek(g_pad_q, &st, 0) == pdTRUE) {   /* 永远读最后一次已知状态 */
            /* ... 算 omega_pan / omega_tilt，变化才打印 ... */
        }
        vTaskDelay(pdMS_TO_TICKS(20));                 /* 50 Hz */
    }
}
```

> 控制环周期取 **20 ms（50 Hz）**：手柄最快 ~32 Hz，50 Hz 足够把每个包都用上，
> 又不至于空转烧 CPU。**注意 50 Hz 是"采样保持"的节奏，不是"接收"的节奏** ——
> 没有新包的那些周期读到的都是同一个值，不打印、也不产生任何副作用。


### 6.3 按键映射（本阶段只打印事件）

| 按键 | 用途 | 备注 |
|---|---|---|
| 左摇杆 | pan 角速度 `ω_ref` | 松手即停 |
| 右摇杆 | tilt 角速度 `ω_ref` | 松手即停 |
| **Start** | 模式切换（MANUAL ↔ AUTO，三期） | — |
| **L1 + R1 + Start** | **急停** | 用组合键避免误触 |
| **Home** | ❌ **禁用** | **长按 3 秒会关手柄电源** |

### 6.4 验收（M2c）

| 编号 | 验收项 | 判定 |
|---|---|---|
| AC-BLE-7 | 左摇杆→pan、右摇杆→tilt；**松手 `ω_ref` 归零**（松手 = 一次状态变化 = 会来一个 `0x80` 的包，所以这里归零是**收到包**导致的，不是超时） | ✅ 左杆 X→pan、右杆 Y→tilt，两边都能扫到 ±60.00 并回 `0.00` |
| AC-BLE-8 | **关掉手柄电源 / 走远断开** → 系统进安全态，`ω_ref` 立刻归零（对应 PRD SR-05） | ✅ 长按 Home 关机 → `disconnect` → 打出一行全零（`push_center()`） |
| AC-BLE-9 | **摇杆推到一半停住不动** → `ω_ref` **保持不变**，不得归零或跳变 | ✅ 见下方"怎么验的" |
| AC-BLE-10 | 满量程对应**正好 ±`OMEGA_MAX_DPS`** | ✅ 实测 `pan=-60.00` / `tilt=+60.00`（见下方踩坑） |
| AC-BLE-11 | **死区边界**：摇杆停在中心附近 1~2 格（`0x80±10`）时 `ω_ref` 应为 0 | ⬜ **尚未验过** |

**AC-BLE-9 怎么验的（2026-09-19）**：这个验收项有个麻烦 —— **"成功"的表现就是"什么都没发生"**
（值没变 → 不打印），所以日志里根本没有"我顶住了 10 秒"这件事的直接证据。

**能证伪的判据是这个不变式**：

```
pan == 0  ⇔  Lx == 128        tilt == 0  ⇔  Ry == 128
```

**只要实现里藏着"超时归零"，就必然出现 `pan=0` 而 `Lx≠128` 的行**
（摇杆明明推着，ω 却被超时清成 0）。实测整份日志 **16 行全部满足，0 违反**。

> **教训**：设计验收项时要保证"做对了"和"做错了"在**观测上可区分**。
> "一行都不打印"这种成功标志不可观测 —— 必须补一条**有反向证据**的不变式，
> 否则这个验收项永远只能是"我没看到问题"，而不是"我验证过没问题"。

**踩到的坑：满量程打出 `±60.47` 而不是 `±60.00`。**
原因：归一化统一用了 `/127`，但中心到 0 是 **128** 个计数、中心到 255 是 **127** 个，
负半轴被放大成 `-1.0079`。**中间行程完全看不出来**，只有推到顶才暴露。
修法是分母按方向取（见 §6.2）。

---

## 6.5 全流程总览（回看 M2a–M2c）

> 前三节按里程碑分开写，代码也按职责拆在 5 个文件里 —— 每处都短，
> 但代价是**没有任何一个地方能一眼看见全流程**。这一节把这条线补回来：
> 排错时先在这里定位"卡在哪一段"，再进对应文件看实现。

### 完整流程（按执行顺序）

```
【启动】app_main()
   nvs_flash_init → nimble_port_init → pad_input_init()   ← 建覆盖队列 + 起 pad_m2c 任务
   → 挂 3 个协议栈回调(reset/sync/store) → peer_init
   → 设设备名 → ble_store_config_init → nimble_port_freertos_init(pad_host_task)
        └─ 协议栈跑起来、同步完成 → 回调 sync_cb = pad_on_sync() → pad_link_scan()

【扫描 → 连接】pad_link_scan()    ble_gap_disc(主动扫描、不过滤重复)
   └─ 每收到一包广播 → pad_gap_event(DISC) → pad_scan_on_disc()
        └─ 认出 CodexPad（先靠名字记住它的地址）→ pad_link_connect(addr)
             └─ ble_gap_disc_cancel() → ble_gap_connect(..., pad_gap_event)
   └─ 连上 → pad_gap_event(CONNECT) → on_connect()
        └─ print_conn_desc + peer_add + pad_gatt_start(conn_handle)

【发现 → 订阅】pad_gatt_start()
   ble_gattc_disc_all_svcs ──► on_svc_disc ×N              收齐服务表（6 个）
        └─ disc_next_svc(): 逐个服务 disc_all_chrs → on_chr_disc ×N  ← 记下 0xFFA1 的 val_handle
             └─ 全部服务翻完 → disc_all_dscs(0xFFA1 的 val_handle) → on_dsc_disc
                  └─ 找到 0x2902 CCCD → ble_gattc_write_flat(CCCD, {01 00})  写 1 = 打开 notify
                       └─ on_cccd_write   ✔ 订阅完成，手柄开始推数据

【数据流】手柄推 notify → pad_gap_event(NOTIFY_RX)
   └─ os_mbuf_copydata → pad_input_on_notify(buf, len)
        └─ len≠8 丢弃；否则 memcpy → push_state()  ==========★ 唯一一处跨任务
   pad_m2c 任务（另一个 task，50 Hz）xQueuePeek → 归一化+死区 → ω_ref → 变化才打印

【掉线】pad_gap_event(DISCONNECT) → on_disconnect()
   peer_delete + pad_gatt_reset + pad_input_center() + pad_link_scan()  ──★ 回到扫描，闭环
```

### 三个必须记住的结构事实

**① 整套 BLE 只跑在 <ins>一个</ins> task 里。**

`app_main()` 不是常驻的，它初始化完就退出。真正长期运行的只有两个 task：

| task | 谁创建 | 干什么 | 绑核 |
|---|---|---|---|
| `nimble_host` | `app_main.c:79` → IDF 的 `nimble_port_freertos.c:43` | `nimble_port_run()` 协议栈主循环，**以及全部 GAP/GATT 回调** | **核 0（PRO_CPU）**，`CONFIG_BT_NIMBLE_PINNED_TO_CORE=0`，栈 4096，优先级 21 |
| `pad_m2c` | `pad_input.c:165` | 50 Hz 控制环：读队列 → 算 `ω_ref` | 未绑（`xTaskCreate`）；将来与增稳环一起进 **核 1（APP_CPU）** |

> ⚠ `app_main.c:79` 传进去的 `pad_host_task` **只是个回调函数指针**，不是 task 本体。
> task 是 IDF 的 `esp_nimble_enable()` 建的，名字叫 `nimble_host`。
> 所以 `pad_host_task` 第一行那句 `"BLE Host 任务已启动"` 是在**新建的那个 task 里**打印的。

**推论（这条最值钱）**：`pad_scan.c` / `pad_link.c` / `pad_gatt.c` 里的代码
**不是三条并行流程**，而是同一个 task 的**不同分支** —— 全都在 `pad_gap_event()`
那个 `switch` 底下。**文件边界 ≠ 运行时边界。**
这就是"拆成 5 个文件之后反而串不起来"的根源。

**② 整个工程只有一处跨任务。**

`pad_input.c` 那个长度 1 的覆盖式队列之所以存在，**唯一理由**是：
`pad_input_on_notify()` 跑在 `nimble_host`（核 0）上，而 `pad_m2c`（核 1）是另一个 task。
理解了 ①，② 就自明 —— BLE 侧内部所有调用都是同 task 直调，**不需要任何同步**。

**③ 发现链是一条链，不是一个循环。**

`services → 每个 service 的 chars → 0xFFA1 的 dscs → 写 CCCD`，每级在自己的
`BLE_HS_EDONE` 里交给下一级（`disc_next_svc()`）。它看着绕，只因为 NimBLE 是
**异步回调式**的：每级都是「发一个请求 → 回调 N 次（每次给一个对象）→ 最后回调
`EDONE`」，没法写成一个 `for` 循环。

### 怎么用这张图

- **排错**：先判断卡在哪一段，再只看那一段 —— 日志在校验前就断了 ⇒ GAP（没连上）；
  连上了但一直没有 `[ω_ref]` ⇒ GATT（没订上 notify）。
- **加功能**：要按键触发动作，改 `pad_input.c` 的 `pad_m2c`（那里已经在解按键）
  —— 它在核 1，跟 BLE 无关，改它不会碰坏协议栈。

---

# 7. M2d —— 下一棒（本里程碑结束）

**里程碑终点**：串口能稳定打印解析好的摇杆/按键，`ω_ref` 数值合理。

**再接电机要先买一颗 3.3V CAN 收发模块**（SN65HVD230 / TJA1051 类，
PRD §3.1 里唯一需要采购的关键件）。买了之后：

```
摇杆 → ω_ref → 差速逆运动学（tools/gimbal.py 已验证）
            → CAN 帧（tools/zdt_can.py 已验证）→ 电机
```

**运动学与 CAN 帧格式都是现成的**（PC 端已实测确认），那一步是"搬运"而不是"探索"。

---
---

# 附录 A —— 协议速查

> **写代码时来这里查。** 全部常量都是从厂商开源库源码逐条读出来的，可直接作为实现依据。
> 来源：`github.com/CodexPad/` 下的 `codex_pad_s10`、`codex_pad_arduino_lib`、
> `gamepad_codec_arduino_lib`、`robust_frame_arduino_lib`、`gamepad_input_arduino_lib`。

## A.0 ⚠ 这不是标准 BLE-HID 手柄

厂商 README 原文：

> **⚠️ 这不是一款"即插即用"的通用游戏手柄**
> CodexPad-S10 **不是 BLE-HID 设备**，不会像 Xbox、PlayStation 那样被操作系统自动识别。
> **❌ 不能**直接连上 Windows/macOS/Linux 或手机就去打游戏；
> **✅ 必须**通过**代码手动建立蓝牙连接**，并自行解析上报数据。

| 影响 | 说明 |
|---|---|
| **不能用 `esp_hid_host`** | 那是连 HID 设备的组件 |
| **改用 NimBLE GATT 客户端** | 主动连手柄的自定义 GATT 服务，订阅 notify，自己解析二进制帧 |
| **好消息** | 协议**是公开的**（开源库 + 手册齐全），不用逆向；而且"自定义协议"比"HID 描述符"**更好处理**（帧格式固定） |

## A.1 设备与连接

| 项 | 值 |
|---|---|
| 广播名称 | `CodexPad-` 开头（如 `CodexPad-XXXX`） |
| 蓝牙版本 | BLE 5.3，**仅作从机（peripheral）** |
| BD_ADDR | 印在**手柄背面中央标签**上，格式 `XX:XX:XX:XX:XX:XX` |
| 输入规格 | **17 个按键 + 2 个双轴摇杆**，摇杆 8 位（0~255） |
| 开机 | 短按中央 **Home 键**，1 号蓝灯慢闪 = 正在广播 |
| 已连接 | 1 号蓝灯**常亮** |
| ⚠ 广播超时 | 慢闪**超过 1 分钟**无连接 → 自动关机 |
| ⚠ Home 长按 3 秒 | **关机** —— 所以 Home 不能做功能键 |

## A.2 GATT 结构（2026-09-19 全表实测）

```
0x1800  GAP           handle  1.. 9   0x2A00 设备名 / 0x2A01 外观 / 0x2A04 首选连接参数 / 0x2AA6
0x1801  GATT          handle 10..13   0x2A05 Service Changed（indicate）
0x1804  Tx Power      handle 14..17   0x2A07 发射功率
0x180A  Device Info   handle 18..30   0x2A24 型号 / 0x2A25 序列号 / 0x2A26 固件版本
0xFFA0  输入服务       handle 31..35   0xFFA1 ← 输入数据流（notify）
0xFFE0  厂商私有       handle 36..40   0xFFE1 ← 文档没有的服务，见 A.6
```

| 服务 | UUID | 特征 | UUID | 属性 | 用途 |
|---|---|---|---|---|---|
| **输入服务** | **`0xFFA0`** | **输入特征** | **`0xFFA1`** | `0x10` notify | **输入数据流全在这里** |
| 厂商私有 | `0xFFE0` | — | `0xFFE1` | `0x1E` 读\|写\|写无响应\|notify | 疑似控制点/输出报告，见 A.6 |
| 通用访问 GAP | `0x1800` | 设备名 / 外观 | `0x2A00` / `0x2A01` | `0x02` 读 | 读设备名 |
| 设备信息 | `0x180A` | 型号 / 序列号 / 固件版本 | `0x2A24` / `0x2A25` / `0x2A26` | `0x02` 读 | 读固件版本 |
| 发射功率 | `0x1804` | 发射功率 | `0x2A07` | `0x0E` | `0x2A07`（−16~+6 dBm） |

- `0xFFA1` 下有两个描述符：**CCCD `0x2902`（handle 34）、`0x2901`（handle 35）**。
  订阅写的是 **handle 34**，`{0x01, 0x00}`。
- **文档早期版本写的 `0x180F`（电池服务）实测不存在** —— 没有电池特征，别指望读电量。
- 实测**连接耗时约 260 ms**，全表发现（6 个服务 × 逐个翻特征）约 1.8 s。

**连接流程**（照抄官方 `codex_pad.cpp` 的顺序）：

1. BLE 主机初始化（设备名任意，官方用 `CodexPadClient`）
2. 连接目标地址
3. （可选）读 `0x1800/0x2A00`（设备名）、`0x180A/0x2A26`（固件版本，实测 `2.3.2`）
4. `getService(0xFFA0)` → `getCharacteristic(0xFFA1)` → 确认 `canNotify()` → **订阅 notify**
5. 收到通知 → 按 A.3 取值（**没有帧要解**）
6. **不需要配对/绑定**，直接连接即可

## A.3 notify 载荷格式（实测：裸 8 字节，无封装）

> ⚠ **2026-09-19 实测更正**：早期版本按厂商 `robust_frame` 库写了
> `0xAA…0x55` + 转义 + CRC8 的帧格式。**那只手柄的 notify 根本不是这个格式**，
> 一帧都解不出来（白等 `0xAA` 等了一整轮）。

**实测载荷 = 定长 8 字节，直接是状态结构体本身，没有任何封装：**

```
┌──────────────────┬────┬────┬────┬────┐
│  buttons (u32 LE) │ Lx │ Ly │ Rx │ Ry │
│       4B          │ 1B │ 1B │ 1B │ 1B │     共 8 B
└──────────────────┴────┴────┴────┴────┘
```

```
00 00 00 00  80 80 80 80      静置：无按键，四轴回中
00 00 01 00  80 80 80 80      Home 键按下 → buttons=0x10000
00 00 00 00  5e 80 80 80      推左摇杆向左 → Lx 128→94
```

- **没有帧头/帧尾、没有转义、没有 CRC** → 不需要写编解码器，`memcpy` 即可。
- **长度恒为 8**。长度不是 8 就当异常丢弃（不要试图"容错解析"）。

> 顺带记录：CRC-8/SAE-J1850（`poly=0x1D, init=0xFF, xorout=0xFF`，MSB-first）
> 的按位实现已用标准向量验过（`crc8("123456789") == 0x4B`），**但本项目用不到**
> —— 没有帧，就没有 CRC 要校验。留着只为以后万一走 `0xFFE1` 时能直接用。

## A.4 载荷字段与键位表

```c
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

**轴的方向约定（2026-09-19 实测）**：两杆的 **上/前方向 = 数值增大**，回中 = `0x80`，
满量程 `0x00`/`0xFF` 都能踩到（8 位，无死区，松手能稳定回 128 附近）。

**按键位（17 键）**：

| 位 | 按键 | 位 | 按键 |
|---|---|---|---|
| `1<<0` | 上（十字键 Up） | `1<<9`  | L2 |
| `1<<1` | 下（Down） | `1<<10` | L3（左摇杆按下） |
| `1<<2` | 左（Left） | `1<<11` | R1 |
| `1<<3` | 右（Right） | `1<<12` | R2 |
| `1<<4` | □ Square/X | `1<<13` | R3（右摇杆按下） |
| `1<<5` | △ Triangle/Y | `1<<14` | Select |
| `1<<6` | ✕ Cross/A | `1<<15` | Start |
| `1<<7` | ○ Circle/B | `1<<16` | **Home（⚠ 勿作功能键）** |
| `1<<8` | L1 | | |

## A.5 扫描响应里带按键状态（额外收获）

> ⚠ **2026-09-19 实测更正**：按键状态**不在 ADV_IND（主动广播）里，在 SCAN_RSP（扫描响应）里**。
> 早期版本这里写的是"广播里"，说法不准确，会把人带偏（实测时为此误判了整整一轮）。
>
> - 手柄的 **ADV_IND 只有两个 AD 结构**：`Flags(0x01)=0x06` + `Complete Local Name(0x09)="CodexPad-S10"`。
>   **没有任何厂商数据**，所以纯被动扫描永远读不到按键。
> - 必须**主动扫描**（扫参数 `passive=0`，让控制器发 SCAN_REQ）才能拿到 SCAN_RSP，
>   厂商数据在那里面（实测速率见下表）。

手柄的**扫描响应里的 Manufacturer Specific Data** 包含当前按键状态，
所以**不连接也能读到按键**：

```c
#pragma pack(push, 1)
struct ManufacturerSpecificData {
    uint16_t company_id;                      // == 0xFFFF
    uint8_t  header[8];                       // == "CodexPad"
    uint8_t  version_major, version_minor, version_patch;
    uint32_t button_state;                    // 当前按键位，同 A.4 的表
    uint8_t  button_states_duration_seconds;  // 已保持秒数
};
#pragma pack(pop)
```

**识别设备必须按 BD_ADDR，不能按名字**：SCAN_RSP 里**没有 name 字段**，
只按名字过滤会把所有扫描响应全丢掉（这一步踩过坑，见 §4.2）。

**实测速率（2026-09-19，ESP32-S3 + NimBLE，手柄开机后持续扫描）**：

| 事件 | 开机初期 | 约 1 分钟后 | 说明 |
|---|---|---|---|
| `0x00` ADV_IND | ~28 Hz | ~15 Hz | **不含按键数据** |
| `0x04` SCAN_RSP | ~19 Hz | ~5 Hz | **按键数据的唯一来源就是它** |

结论：**按键更新率只有 5~19 Hz 且随时间衰减**，而且**完全没有摇杆模拟量**
（这个结构里就没有轴数据）。所以**广播通道不足以做主控输入**，
只适合做低速辅助。摇杆 + 有保障的速率必须走连接后的 notify（正文 M2b）。

**两个用途**：

1. **"按键掩码扫描连接"** —— 官方特色功能：让手柄按住某组合键，主机扫描时
   只连"按键状态恰好等于掩码"且 RSSI 最高的那台。多手柄环境防误连、
   换手柄不用改代码里硬编码的 BD_ADDR。
2. **M2a 阶段的最小验证** —— 连都不用连，扫到就能确认
   "板子蓝牙 OK + 手柄在广播 + 协议理解正确"。

**2026-09-19 实测结论（M2a 验收）**：A.4 的 17 键位表**全部实测通过**，
包括 `1<<13`（R3，右摇杆按下）和 `1<<8 | 1<<11`（L1+R1 同时按，两位同时置位）。
手柄固件版本实测 `fw = 2.3.2`，`held` 字段确为"已保持秒数"。

## A.6 未解的部分：`0xFFE1`

GATT 全表扫描时发现了一个**文档和厂商库里都没提的服务 `0xFFE0`**：

```
[GATT] 服务 uuid=0xFFE0 handle=36..40
[GATT]   特征 uuid=0xFFE1 def=37 val=38 props=0x1E (notify)
```

`props = 0x1E` = **读 | 写 | 写无响应 | notify**，是个双向特征。三件事都还没做：

| 待验证 | 怎么试 | 如果成立意味着什么 |
|---|---|---|
| 能不能**写** | 往 val_handle=38 写几字节，看手柄反应 | 可能是**震动/灯效**等输出通道 |
| notify 会不会**主动发** | 订阅它的 CCCD，静置观察 | 可能是**另一路状态流** |
| 是不是 `0xAA…0x55` **帧协议的入口** | 写一条带帧的命令看是否被接受 | 若是，A.3 那套协议可能只在**写方向**用 |

**为什么值得记一笔**：如果 M2b/M2c 之后发现需要"主机→手柄"方向的通信
（震动反馈、切换上报模式、改采样率），`0xFFE1` 是唯一的候选。
**当前里程碑用不到**，不阻塞任何验收项。

## A.7 引脚分配与复用机制

板子 **YD-ESP32-S3-N16R8**（16 MB Flash + 8 MB PSRAM）。**本里程碑 BLE 走芯片内部射频，
不占任何 GPIO** —— 下面这些是为后面的 CAN / IMU / 泰山派 / OLED 准备的。

### A.7.1 先搞懂：ESP32 的引脚复用是「两层」，STM32 只有一层

| | STM32 / TI | ESP32-S3 |
|---|---|---|
| 复用层级 | 一层（AF0~AF15 复用器） | **两层** |
| 外设能换脚吗 | 只能换到该外设被设计支持的**那几个固定脚** | 数字外设**任意 GPIO** 都能接 |
| 典型表现 | `USART1_TX` 只能是 PA9 / PB6 | UART1 TX 可以放 17 号脚，也可以放 4 号脚 |

- **第一层 IO_MUX（直连）**：每个引脚出厂绑好 1~2 个默认功能，走芯片内部直连通道。
  这一层才等效于 ST 的那套复用器。
- **第二层 GPIO 交换矩阵（交叉开关）**：把外设信号搬到**任意**引脚上。
  **这是 ST / TI 没有的东西。**

官方原文（ESP-IDF v5.5.5 自带中文文档，**离线可查**）：

> 出处 `D:\esp\v5.5.5\esp-idf\docs\zh_CN\api-reference\peripherals\gpio\esp32s3.inc:12`
>
> ESP32-S3 芯片具有 45 个物理 GPIO 管脚（GPIO0 ~ GPIO21 和 GPIO26 ~ GPIO48）。
> 每个管脚都可用作一个通用 IO，或连接一个内部外设信号。**通过 GPIO 交换矩阵、
> IO MUX 和 RTC IO MUX，可配置外设模块的输入信号来源于任何的 GPIO 管脚，
> 并且外设模块的输出信号也可连接到任意 GPIO 管脚。**

> **面试可讲**：这是 ESP32 与 STM32 在引脚复用上最本质的区别 —— 多了一层可编程交叉开关，
> 代价是信号多走 1~2 个 APB 时钟周期（约 25 ns），换来外设引脚完全自由。

### A.7.2 哪些能路由、哪些不能

分界线是 **数字 vs 模拟** —— 交叉开关只能搬数字信号。

| 外设 | 能自由路由 | 说明 |
|---|---|---|
| UART（TX/RX/RTS/CTS） | ✅ | UART0/1/2 都能接任意 GPIO |
| I2C（SCL/SDA） | ✅ | S3 的 IO_MUX 表里没给 I2C 留直连，**天生只能走矩阵** |
| SPI2 / SPI3 | ✅ | 信号全过矩阵 |
| I2S / LEDC(PWM) / RMT / TWAI(CAN) / SDIO / PCNT | ✅ | 都没有固定脚 |
| **ADC（模拟输入）** | ❌ | **ADC1 = GPIO1~10，ADC2 = GPIO11~20，硬件锁死** |
| **触摸（Touch）** | ❌ | GPIO1~14 |
| **USB** | ❌ | 有专用 PHY，锁死 GPIO19/20 |
| **SPI0 / SPI1** | ❌ | 板载 Flash / PSRAM 专用 |

**代价**：走矩阵多 1~2 个 APB 时钟周期（约 25 ns）。
官方给的阈值是 **> 40 MHz 的信号才需要用 IO_MUX 直连脚**（`uart.rst:448`）——
115200 波特率、400 kHz I2C 完全无感，随便挑脚。

### A.7.3 本板 45 个 GPIO 的限制

| 类别 | 引脚 | 说明 |
|---|---|---|
| ⚠ **禁用** | **GPIO26 ~ GPIO37** | 26~32 是 SPI0/1，**板上根本没引出来**；**33~37 是 Octal 的 `SPIIO4~7`/`SPIDQS`**。N16R8 就是 Octal PSRAM，官方明确"不推荐用于其他用途" |
| Strapping | GPIO0 / GPIO3 / GPIO45 / GPIO46 | GPIO0 拉低进下载模式。⚠ 板子自带文档只写了 0/45/46，**官方 IDF 文档把 GPIO3 也算进来**，以官方为准 |
| USB 专用 | GPIO19 / GPIO20 | 原生 USB-OTG / USB-JTAG |
| ADC1 | GPIO1 ~ GPIO10 | 做电池检测**只能用这段** |
| ADC2 | GPIO11 ~ GPIO20 | ⚠ **WiFi 一开就废**（板子文档 `:48` 也提醒了），别用 |
| 触摸 | GPIO1 ~ GPIO14 | — |
| **空闲可用** | GPIO1~9、12~16、21、38~42、47 | 扣掉上面那些之后剩下的 |

### A.7.4 本项目引脚分配

| 用途 | 引脚 | IO_MUX 直连 | 状态 |
|---|---|---|---|
| UART0 调试/下载（板载 CH340） | GPIO43 TX / GPIO44 RX | ✅ | **固定、不可占用**。板上那两颗 TX/RX 指示灯就挂在这 —— 占用即失去串口日志 |
| UART1 → IMU（汇电籽-601） | GPIO17 TX / GPIO18 RX | ✅ 直连 | 规划 |
| UART2 → 泰山派 RK3576 | GPIO10 TX / GPIO11 RX | ❌ **无直连**（`soc/uart_pins.h` 里是 `-1`），走矩阵 | 规划，⬜ 待上板确认 |
| **I2C → OLED（SSD1306）** | **GPIO8 SDA / GPIO9 SCL** | ❌ 走矩阵 | 0x3C、400 kHz，**需外挂 4.7 kΩ 上拉到 3V3**（内部上拉太弱） |
| CAN（TWAI）→ 电机收发器 | **待定** | ❌ 无直连 | ⬜ 需先买收发器 |
| 板载 RGB（WS2812） | GPIO48 | ❌ 走矩阵（RMT） | ✅ 已验证，工程 `D:\esp\led_blink` |
| USB-OTG 原生 | GPIO19 D- / GPIO20 D+ | 专用 PHY | 固定 |
| 电池电压检测（若做） | 只能从 **ADC1** 里选 | 硬件锁死 | ⬜ 未规划。**8/9 已被 I2C 占掉，ADC1 只剩 GPIO1~7、10** |

**CAN 引脚怎么选**（两条约束，选之前先定要不要做电池检测）：

1. **要做电池检测** → 必须留 ADC1（GPIO1~10），CAN 就别占 1~10，建议 **GPIO38 / GPIO47**
2. **不做电池检测** → **GPIO4 / GPIO5** 最舒服（离 17/18、10/11 都远，布线不打架）

### A.7.5 排错技巧：查某根脚实际接了什么

```c
gpio_dump_io_configuration(stdout, (1ULL << 4) | (1ULL << 18) | (1ULL << 26));
```

输出里看三个字段：

- `FuncSel: 1 (GPIO)` = 走 GPIO 矩阵；`FuncSel: 0 (IOMUX)` = 走 IO_MUX 直连
- `GPIO Matrix SigOut ID: 256` = 具体的矩阵信号编号，
  完整清单在 `components/soc/esp32s3/include/soc/gpio_sig_map.h`（**442 条**）
- `**RESERVED**` = 被 flash / PSRAM 占用的脚，**别碰**

---

# 附录 B —— 排错表

| 现象 | 可能原因 | 处理 |
|---|---|---|
| **扫不到手柄** | ① 手柄没开机 ② **已过 1 分钟自动关机** ③ 被手机/电脑连走了 | 短按 Home 重新开机；**先在终端启动扫描、再开机**；关掉附近已连的蓝牙主机 |
| 扫到但连不上 | BD_ADDR 写错 / RSSI 太差 | 对照背面标签核对地址；靠近板子 |
| 连上又立刻断 | 供电不足（USB 口带不动）/ 手柄电量低 | 换 USB 口或带供电的 Hub；给手柄充电 |
| **收到 notify 但一个字都解析不出来** | 按错误的假设（`0xAA…0x55` 帧格式）在等帧头，而实际载荷是**裸 8 字节** | **别猜格式，先 dump 原始 hex**（§5.1）；实测就是 8 字节，长度不符直接丢 |
| **手一停就没有 notify 了** | 这是**正常行为**，不是掉线 —— 手柄是变化驱动，状态不变就不发 | **不要把看门狗挂在 notify 上**；掉线靠 `BLE_GAP_EVENT_DISCONNECT` 判断（§5.3） |
| 数值乱跳 | 把扫描响应按键位和 notify 数据混用了 | 明确区分两个数据来源 |
| **读不到电池电量** | 手柄**没有电池服务**（`0x180F` 实测不存在） | 读不到是正常的，别在这上面花时间（§A.2）|
| 扫描正常但**永远读不到按键** | `on_disc` 只按**名字**认设备，而 SCAN_RSP 里没有 name 字段 | 先按名字认出地址，之后**按地址**匹配（§4.2）|
| 手柄用一会儿自动关机 | **广播超时**（未连接超 1 分钟）或电量低 | 尽快连接；充电 |
| `idf.py` 找不到 / 报错缺 target | ESP-IDF 环境变量没加载 | 先激活环境（§1.1）：双击桌面 `IDF_v5.5.5_Powershell`，或手动 `. D:\esp\tools\Microsoft.v5.5.5.PowerShell_profile.ps1` |
| 打印 `MSys/Mingw is no longer supported` 后什么都不做 | 在 Git Bash / MSYS2 里跑了 `idf.py` | 换 PowerShell 或 CMD；v5.5 已不支持 MSys |
| VS Code 里 ESP-IDF 图标全是灰的 / 找不到安装 | `idf.eimIdfJsonPath` 没指向本机 | 见 §1.2；本机已设成 `D:\esp\eim_idf.json` |
| 编译提示空间不足 | Flash 大小没设成 16 MB | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` + 分区表 |
| 行为诡异 / 随机崩溃 | 误用了 GPIO26~32 | 检查引脚，这些是 Flash/PSRAM 专用 |
| 设备管理器没有 COM 口 | 缺 CH340 驱动 / USB 线只能充电 | 装 CH340 驱动；换数据线；换板上另一个 USB 口 |
| 烧录报 `No serial data received` | 没进下载模式 | 按住 `BOOT` → 点一下 `RST` → 松开 `BOOT` |
| CMake/Ninja 报奇怪的路径错 | **工程路径含中文或空格** | 把工程挪到 `D:\esp\` 这种纯英文路径 |

---

# 附录 C —— 备选路线（卡住时的逃生路线）

| 方案 | 做法 | 优点 | 缺点 | 建议 |
|---|---|---|---|---|
| **主线：ESP-IDF + NimBLE** | 自己写 GATT 客户端 + 帧解析 | 正式技术栈，无返工；面试能讲"自己实现 BLE GATT 客户端" | 代码量约 400~600 行，NimBLE C API 较啰嗦 | ✅ **采用** |
| 备选 A：arduino-esp32 | 把 Arduino 作为 IDF 组件引入，直接用官方 `CodexPad` 库 | **最快跑通**（约 30 分钟） | 混框架、体积大、面试讲不清"哪里是你写的" | 卡住时的逃生路线 |
| 备选 B：MicroPython | 刷 MicroPython 固件，跑官方 `codex_pad_mpy_lib` | 最快验证链路 | 非项目技术栈，等于白做一遍 | 只用于**临时证伪** |
| 备选 C：PC + `bleak` | 电脑上读手柄（**第 2 步已经做了**） | 不需要板子 | 只是诊断工具 | ✅ **已经作为二分法基准** |
| 备选 D：换标准 BLE-HID 手柄 | 回到 `esp_hid_host` 路线 | PRD 早期方案 | 要重买手柄，且 HID 描述符差异同样是坑 | 仅在 A/B/C 全失败时考虑 |

**本方案的最大风险已从"协议未知"降级为"移植工作量"** —— 因为协议已经完整拿到（附录 A）。

**仍需实测确认的未知项**：

- ~~摇杆物理方向与符号~~ → ✅ 2026-09-19 实测：两杆**上/前 = 数值增大**，回中 `0x80`（§A.4）
- ~~实际的 notify 帧率~~ → ✅ 2026-09-19 实测：动时 **≈32 Hz**，静置 **0 Hz**（变化驱动，§5.4）
- `0xFFE1` 到底是干什么用的（§A.6，**不阻塞当前里程碑**）
- 双手柄/多设备环境下的干扰情况

---

# 附录 D —— 官方资源索引

| 资源 | 地址 | 用途 |
|---|---|---|
| S10 手册（GitHub 镜像） | `github.com/CodexPad/codex_pad_s10` | 产品规格、指示灯、BD_ADDR、开关机 |
| 连接指南（原生 BLE） | `github.com/CodexPad/codex_pad_guide` | 两种连接方式说明 |
| Arduino 库（连接） | `github.com/CodexPad/codex_pad_arduino_lib` | GATT UUID、连接流程的权威对照 |
| 帧编解码 | `github.com/CodexPad/gamepad_codec_arduino_lib` | 载荷格式（⚠ 其 `robust_frame` 封装**实测未在这只机器上出现**，见 A.3） |
| 帧框架 + CRC | `github.com/CodexPad/robust_frame_arduino_lib` | 0xAA/0x55/转义/CRC8 表（**本项目用不到**，留作 `0xFFE1` 的备用参考） |
| 输入类型定义 | `github.com/CodexPad/gamepad_input_arduino_lib` | `State` / `Button` / `Axis` |
| PC 端 Python 库 | `github.com/CodexPad/codex_pad_bleak_lib` | **第 2 步的诊断对照** |
| MicroPython 库 | `github.com/CodexPad/codex_pad_mpy_lib` | 备选路线 B |
| 原始手册（Gitee） | `gitee.com/CodexPad/codex_pad_s10` | 用户提供的原始链接 |
| ESP-IDF `blecent` 示例 | ESP-IDF `examples/bluetooth/nimble/blecent` | **第 3 步的工程底板** |

> **网络提示**：本机 `gitee.com` 手工链接被网络策略拦截，`raw.githubusercontent.com`
> 也解析不了，但 `curl api.github.com`（GitHub contents API +
> `Accept: application/vnd.github.raw`）能通，`WebFetch` 对 gitee/github 都被拦。
> 需要看某个文件时优先走 `api.github.com`。
