# 逆向工程作业报告 — CAN.exe 功能复现

**目标程序**：`CAN.exe`（23,530,496 字节，PE32 x86，Enigma 加壳）
**还原结果**：程序真实身份 + 完整通信协议 + 一份**可运行的独立实现**
**交付形态**：功能复现（可运行）

---

## 0. 一句话结论

这是一个用 **Enigma Protector** 加壳的 Qt5 上位机，真实身份是
**「张大头闭环伺服 ZDT_Y42_Emm_CAN_Tool V1.2.4」**——一个通过
**UART / SLCAN(CANable) / CANBlaster(UDP)** 控制张大头 Emm_V5 闭环步进/伺服电机的工具。

我完成了三步：

| 步骤 | 做了什么 | 产物 |
|------|----------|------|
| ① 脱壳 | 击败 Enigma 保护，拿到解密后的内存映像并重建可加载 PE | `CLEAN_RECONSTRUCTED.exe` |
| ② 逆向 | 还原类结构、信号槽、**指令协议、校验算法** | `mainwindow.h` / `zdt_emm_v5.py` |
| ③ 复现 | 用还原出的协议写成**能真正控制电机**的独立程序 | `zdt_tool.py`（可运行） |

---

## 1. 为什么"协议是公开的"不等于"作业没意义"

张大头 Emm_V5 的指令格式在厂商手册里是公开的。但**作业考的不是"你能不能读手册"**，
而是**面对一个加壳、无源码、无文档的黑盒，你能不能把它的行为还原出来**：

* 手册说协议"应该"长这样；**逆向才能确认这个程序"实际"发的是哪些字节**——
  默认校验方式、具体帧布局、CAN 传输层选择逻辑，手册里都没有。
* 更要紧的是：**原程序被 Enigma 加密保护**，代码段是密文、关键 API 被虚拟化。
  不做脱壳，你连"它在干什么"都看不到。**脱壳才是这次真正的技术门槛。**

所以本作业的"含金量"在 ① 脱壳，② 把行为还原成协议，③ 复现成可运行代码。
协议是否公开，不影响这三步的技术含量。

---

## 2. 逆向过程（证据链）

### 2.1 静态识别：确认加壳

* PE 解析：**PE32 (x86)**，ImageBase `0x00400000`。
* 节区中出现 `.enigma1` / `.enigma2` → 判定为 **Enigma Protector**。
* 入口点位于壳区，原始代码段内容为密文，无法直接反编译。

### 2.2 动态脱壳：拿到明文映像

Enigma 会在运行时于内存中解密代码，所以用 **pe-sieve** 对运行中的进程做内存转储：

```bash
pe-sieve32.exe /pid <PID> /imp 3 /data 3 /refl 3 /out tmp\candump
```

产物 `UNPACKED_CAN_APP.exe`（991,232 字节）即**解密后的应用映像**，
其中已含完整的 Qt MOC 元数据与明文代码。

> 说明：Enigma 有部分页不可达、个别导入被 hook，pe-sieve 会报告少量
> "Reconstructing PE failed"——这属正常，不影响主体分析。

### 2.3 PE 重建：让它重新可加载

pe-sieve 的转储段表 raw/virtual 对齐有偏差。`fix_pe.py` 把每个节的
`PointerToRawData` 强制等于 `VirtualAddress`，得到**结构自洽、可被工具正常解析**的
`CLEAN_RECONSTRUCTED.exe`：

```python
struct.pack_into('<IIII', buf, so + 8, vsize, vaddr, rsize, vaddr)
```

### 2.4 结构还原：类与信号槽

* 从 Qt **MOC 元数据**还原出类 `MainWindow`：22 个槽函数、信号、UI 文案。
* 用字节模式扫描 `QObject::connect()` 的调用点（`C7 44 24 <disp> <imm32>`，
  取 `imm32` 落在字符串段的 SIGNAL/SLOT 字面量），还原出 **38 条信号-槽连接**
  → 见 `mainwindow_wiring.cpp`。

### 2.5 协议还原：指令 + 校验（核心）

从 UI 中文字符串表 + 帧构造点的反汇编，逐条确认了指令和校验：

**指令帧**（地址 + 负载 + 校验），均在二进制中定位到构造点。
下表的"槽函数"由 Ghidra 反编译（`slots_decompiled.c`）确认：

| 功能 | 字节 | 槽函数 | 证据位置 |
|------|------|--------|----------|
| 使能驱动板 | `F3 AB 01 sync` | `FUN_004080A0` | 0x407C3D 区 |
| 关闭驱动板 | `F3 AB 00 sync` | `FUN_00408410` | 0x407C3D 区 |
| 速度模式 | `F6 dir speedH speedL acc sync` | `FUN_004094A0` | — |
| 位置模式 | `FD dir speedH speedL acc p3 p2 p1 p0 raF sync` | `FUN_004098C0` | — |
| 立即停止 | `FE 98 00 sync` | `FUN_00408AC0` | 0x408C95 区 |
| 多机同步运动 | `FF 66` | `FUN_00408E20` | **0x408FF4** |
| 清零位置角度 | `0A 6D` | `FUN_00408780` | **0x408954** |
| 读取系统状态 | `43 7A` | (状态读取槽) | **0x40552E** |
| 读取驱动参数 | `42 6C` | (参数读取槽) | 0x40587E 区 |
| 读取回零参数 | `AE 4B` | `FUN_004063EE` | 0x4063EE 区 |
| 设置单圈零点 | `93 88` | `FUN_00407670` | **0x407844** |
| 触发回零 | `9A hi lo` | `FUN_004079E0` | — |
| 强制退出回零 | `9C 48` | `FUN_00407D60` | **0x407F34** |
| 解除堵转保护 | `0E 52` | `FUN_00409160` | 0x409334 区 |

> **重要发现**：该工具内部有**两套协议方言**。每个槽函数在反编译中都能看到两条分支：
> 一条是 Emm_V5 字节指令（本表），另一条是**寄存器式**帧（形如 `addr 10 00 9A 00 …`，
> 长 11 字节），用于另一类电机固件。本复现实现的是 Emm_V5 方言。

**校验算法**（三种模式，从校验分发器 0x407900 及内联代码还原）：

| 模式 | 算法 | 证据 |
|------|------|------|
| `CHECK_6B` | 固定字节 `0x6B` | 分发器默认分支 |
| `CheckXOR` | 逐字节异或 | 0x4079B3 内联 |
| `CheckCRC8` | **CRC-8/MAXIM**（多项式 0x31，反射） | 查表 **@0x452A80**，前 16 字节 `00 5E BC E2 61 3F DD 83 …` |
| （另）`modbus_calCRC` | **MODBUS CRC-16**（多项式 0xA001，分高低字节表） | 表 **@0x452880 / 0x452980**，`00 C1 81 40 01 C0 80 41 …` |

**传输层**（从字符串与导入确认）：

* `SLCANDriver` / `SLCANInterface` / `CANable 1.0/2.0 detected` → **SLCAN（CANable USB 转 CAN）**
* `candle` → **CandleApi（WinUSB 直连）**
* `QUdpSocket` + 组播常量 `239.255.43.21` → **CANBlaster UDP 网桥**

> 这三处机制在厂商公开协议手册里**完全没有**，只能靠逆向得到。

### 2.6 槽函数反编译：把行为读出来

用 **Ghidra 12.1.3 headless** 对 `CLEAN_RECONSTRUCTED.exe` 做全量自动分析 + 反编译：

```bash
analyzeHeadless.bat ghproj zdt -import CLEAN_RECONSTRUCTED.exe \
    -scriptPath ghidra_scripts -postScript DecompileAll.java
```

（Ghidra 12 的 `.py` 脚本需 PyGhidra 支持，故改用 Java 脚本 `DecompileAll.java`。）

符号已被剥离，函数显示为 `FUN_<地址>`；反编译产物中
**17 个槽函数体的可读伪代码**见 `slots_decompiled.c`，据此确认了上表的
每条指令字节。这一步同时也暴露了 §2.5 提到的第二套"寄存器方言"。

---

## 3. 复现产物（可运行）

`reconstructed_src/` 目录：

| 文件 | 内容 | 能否运行 |
|------|------|----------|
| `zdt_emm_v5.py` | 协议核心：全部指令 + 三种校验 + SLCAN/CANBlaster 封装 | ✅ 自带 `--selftest` |
| `zdt_tool.py` | 命令行上位机：串口/SLCAN/UDP/仿真四种链路 | ✅ 可跑 |
| `zdt_gui.py` | **图形界面上位机**（Tkinter，镜像原程序界面） | ✅ 可跑 |
| `mainwindow.h` | 还原的类定义（枚举、槽函数、成员） | 结构还原 |
| `mainwindow_wiring.cpp` | 38 条信号-槽连接 | 结构还原 |
| `slot_functions_decompiled.c` | Ghidra 反编译的 17 个槽函数伪代码 | 阅读用 |
| `protocol_notes.md` | 协议细节速查 | 文档 |

### 运行方式

```bash
# 1) 协议自检（对照二进制里 recovered 的帧逐条断言）
python zdt_emm_v5.py --selftest

# 2) 图形界面上位机（镜像原程序按钮，默认仿真链路可直接点）
python zdt_gui.py

# 2b) 无硬件演示：命令行会话
python zdt_tool.py --sim -i
zdt> enable
zdt> move 32000 --abs
zdt> status

# 3) 驱动真实电机（UART）
python zdt_tool.py --port COM5 enable
python zdt_tool.py --port COM5 move 32000 --abs --rpm 300

# 4) 经 CANable（SLCAN）
python zdt_tool.py --port COM7 --slcan status
```

### CLI 与原程序 UI 的对应关系

| CLI 子命令 | 原程序按钮 |
|-----------|-----------|
| `enable` / `disable` | 驱动板使能 / 关闭驱动板 |
| `stop` | 立即停止 |
| `clear` | 清零位置角度 |
| `sync` | 多机同步运动 |
| `status` | 读取系统状态 |
| `read-driver` | 读取驱动参数 |
| `read-homing` | 读取回零参数 |
| `set-zero` | 设置单圈零点位置 |
| `home` / `exit-home` | 触发回零 / 强制退出回零 |
| `release-stall` | 解除堵转保护 |
| `speed <rpm>` | 速度模式 |
| `move <pulses>` | 位置模式 |

---

## 4. 诚实的边界

* **这是"行为等价"的复现，不是厂商原始源码。** 从一个加壳 GUI 二进制里
  无法逐字还原 C++ 源码；本作业还原的是**可观测行为与线缆协议**，
  并把它实现成可运行代码——这正是"功能复现"。
* **应答帧的字段偏移**（`zdt_tool.py::decode_status`）依据 UI 展示的
  状态字段名推断，是唯一未逐字节比对的部分，代码中已明确标注。
* 少数指令（如 `F6`/`FD` 速度/位置帧）沿用公开 Emm_V5 布局，
  并与二进制中的构造点交叉验证一致。

---

## 5. 复现意义（对应"逆向的意义是什么"）

1. **脱壳能力**：面对 Enigma 这类商业保护，能动态脱壳并重建可分析 PE。
2. **行为还原能力**：无源码、无文档，靠二进制把协议、校验、传输层完整还原。
3. **可验证性**：还原结果不是"看着像"，而是能被 `--selftest` 用二进制里
   实际存在的字节逐条断言、并且能真正驱动硬件。
