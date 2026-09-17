# 工具脚本使用指南（`tools/`）

二轴差速齿轮云台的联调工具集。用 Python 直连 CAN，绕过官方那两个只能点鼠标的 GUI。

官方上位机 `Y42_Emm_CAN_Tool` 和 `cangaroo` 都是纯 GUI，没法命令行驱动、没法脚本化、
没法在跑动作的同时采数据。这套脚本就是补这个缺口。

> 下文命令都假设在**项目根目录**下执行（即 `python tools/xxx.py`）。
> 在 `tools/` 目录里的话去掉 `tools/` 前缀即可。

---

## 0. 先看这一节（否则后面全是坑）

### 一次性前提

| 前提 | 说明 |
|---|---|
| **适配器独占** | candleLight 适配器同一时间只能被**一个程序**占用。跑任何脚本前先关掉 Y42 上位机 / cangaroo。 |
| **两条命令不能并行** | 同理，`zdt_can.py` 和 `gimbal.py` 不能同时跑在两个终端里。要"边动边看"只能用 `scope.py`（动作跑在它自己的后台线程里）。 |
| **供电** | 实测电源 12V，`43 7A` 读到的总线电压约 11.4V（手册 5.8.2：这是 V+ 过反接二极管后的值）。手册 81 行：工作范围 **10–29V**。12V 下力矩余量比 24V 小，堵转时电压是一条可动的杠杆。 |
| **环境** | 依赖 `python-can / pyusb / libusb-package / gs_usb / matplotlib / numpy / pyqtgraph / PySide6` |

### 环境坑：pip 必须「绕开系统代理」+「用清华镜像」，只做一半会失败

这台机器配了个**系统代理**（pip 通过 `urllib.getproxies()` 从 Windows 注册表读到）。
代理没开时 pip 报 `ProxyError: WinError 10061 目标计算机积极拒绝`，
而 `env | grep proxy` **看不到**任何代理环境变量、`pip config list` 也是空的——别被这个误导。

一次到位的写法：

```bash
HTTP_PROXY= HTTPS_PROXY= http_proxy= https_proxy= NO_PROXY='*' \
  python -m pip install --proxy "" -i https://pypi.tuna.tsinghua.edu.cn/simple <包名>
```

### 环境坑：VS Code 调试会话会锁死适配器

调试会话里程序抛异常后，VS Code **把会话停在异常断点上、进程不退出** → USB 句柄不释放。
之后每次启动都撞 `usb.core.USBError: [Errno 13] Access denied` → 又停在异常处 → 越堆越多。

- 真正的原因会被 `__del__` 里的 `access violation` 连锁崩溃**盖住**，别被那个栈带偏，
  要看**最上面**那行原始错误。
- 强杀进程后固件状态没复位，会变成 `[Errno 32] Pipe error`（控制传输 stall），
  **必须物理拔插 USB 适配器**才能复位。
- 用 `--repeat 0` 这类常驻循环前先想好怎么停；别在 VS Code 里连点多次预设。

---

## 1. 文件总览

| 文件 | 一句话 | 什么时候用 |
|---|---|---|
| `zdt_can.py` | 底层 CAN 命令行工具，20 个子命令 | 读参数、使能、扫描、采样、量延迟 |
| `gimbal.py` | 云台动作序列（差速运动学 + 软限位） | 让云台按姿态动起来 |
| `scope.py` | 实时波形示波器（可边动边看） | 看波形，尤其是运动中 |
| `plot.py` | 离线画图（读 CSV 出 4 张子图） | 复盘已落盘的数据、多组实验对比 |
| `config.py` | 标定参数的管理入口 | 看/初始化参数文件 |
| `gimbal_config.json` | 参数本体（mirror / 零点 / 限位 / 运动默认值） | 改标定值时直接编辑 |

依赖关系（下层是被导入的）：

```
config.py ──┐
            ├──> gimbal.py ──┐
zdt_can.py ─┴────────────────┴──> scope.py
                                     │
                          (sample 产出 CSV) ──> plot.py
```

`zdt_can.py` 是最底层：`gimbal.py` 和 `scope.py` 都 `import` 它来发 CAN 帧。

---

## 2. `zdt_can.py` — 底层 CAN 命令行工具

**意义**：整个工具集的地基。所有 CAN 帧的构造、发送、应答重组都在这里，
上面的 `gimbal.py` / `scope.py` 只是在这个地基上搭运动学和界面。
协议字节逐字节核对过官方固件 `Emm_V5.c` 的 `can_SendCmd`。

### 用法

```bash
python tools/zdt_can.py [--addr 1,2] <子命令> [参数]
```

全局参数：

- `--addr 1,2` — 电机地址，**逗号分隔可指定多个**，命令会依次对每个地址执行
- `--wait 0.4` — 等待应答秒数
- `--retries 2` — 应答失败后的重试次数（0 = 不重试）
- `--verbose` — 打印重试过程

### 子命令分四类

**读**

| 子命令 | 作用 |
|---|---|
| `status` | 读系统状态参数（`43 7A`，31 字节。位置/转速/电流/误差/电压/状态一次全有） |
| `read-driver` | 读驱动参数 |
| `read-homing` | 读回零参数（功能码 `22`） |
| `home-status` | 读回零状态标志（`3B`） |

**控制**

| 子命令 | 作用 |
|---|---|
| `enable` | 使能驱动板；加 `--off` 变失能（松手，可手动摆位） |
| `move` | 位置模式（`pulses` `--rpm` `--acc` `--ccw` `--abs` `--sync`） |
| `speed` | 速度模式 |
| `stop` | 立即停止（`FE 98`） |
| `clear` | 当前位置清零 |
| `set-zero` | 设置单圈零点（`--no-save` 不写电机，掉电丢） |
| `release-stall` | 解除堵转保护（`0E 52`） |
| `set-id` | 修改电机地址并保存（`AE 4B`，**别和 `read-homing` 搞混**） |

**回零**

| 子命令 | 作用 |
|---|---|
| `home [mode]` | 触发回零（`9A <模式> <同步标志>`）。模式 `0` 单圈就近 / `1` 单圈方向 / `2` 无限位碰撞 / `3` 限位 / `4` 回绝对零点 / `5` 回掉电位置 |
| `home-exit` | 强制中断并退出回零（`9C 48`） |
| `home-status` | 读回零状态标志 |

**诊断 / 批量**

| 子命令 | 作用 |
|---|---|
| `scan` | 扫 1..8 号谁在线 |
| `listen` | 被动监听总线 `--secs 3` |
| `raw` | 发原始帧（`--id 0x0100 --data "43 7A 6B"`），排查时用 |
| `sync-all` | 广播触发多机同步（`ID=0x0000`，`FF 66 6B`） |
| `sample` | 定时轮询状态并写 CSV（画波形用） |
| `latency` | 量各读命令的往返延迟分布 |

### 常用例子

```bash
python tools/zdt_can.py --addr 1,2 scan                     # 谁在线
python tools/zdt_can.py --addr 1,2 status                   # 读两台状态
python tools/zdt_can.py --addr 1,2 enable --off             # 两台一起失能（可手动摆位）
python tools/zdt_can.py --addr 1,2 sample --secs 10 --hz 20 --log sample.csv
python tools/zdt_can.py --addr 1 latency --op all --n 50     # 量延迟
```

### `sample` 的细节

边采边写 CSV，带 `#` 元信息行，Ctrl+C 也能留下已采到的数据。
列名（与 MCU 侧日志约定一致，方便两边叠图）：

```
t_s 秒 / addr / bus_mv 毫伏 / phase_ma 毫安 / encoder
target_deg 度 / speed_rpm 转每分(带符号) / pos_deg 度 / err_deg 度
homing_flags / motor_status
```

**空闲基线**（供对照）：两台转速恒 0、位置 ≈ ±0.03°、相电流 12–16mA、
`motor_status=0x03`（使能+到位）、`homing_flags=0x0B`（含 bit3 `Org_CF` 回零失败，
因为从没做过回零——手册 5.4.4 说该位默认应为 0）。

所以**回零是否成功有个现成判据**：跑 `9A` 前后读 `3B`，应从 `0x0B` 变 `0x03`。

### `latency` 的读法

```bash
python tools/zdt_can.py --addr 1 latency --op all --n 50
```

输出 min / p50 / p90 / max / mean，以及由此推出的单台与两台上限 Hz。
`--op` 可选 `status`（`43 7A`，胖）/ `pos` / `speed` / `phase` / `err` / `mstatus`
（后五个是 5.5 单量命令，应答都只有 1 帧），或 `all` 横向对比。

**实测（2026-09-17）**：这些命令全都 ~0.7–1.9ms。

> **排查心法**：如果测出「跟应答长度无关的常数延迟」，先怀疑**主机侧**，不是电机。
> 曾经 `drain()` 就给每条读取白加了 15ms（见下面的"已知的坑"）。

---

## 3. `gimbal.py` — 云台动作序列

**意义**：把"托盘要摆成什么姿态"翻译成两条电机命令。托盘姿态用差速分解：

```
俯仰/仰视俯视 = (θ1 + θ2) / 2   →  两电机世界同向同角度
自转/spin     = (θ1 − θ2) / 2   →  两电机世界反向同角度
```

用多机同步机制保证两轴**同时**动：先分别把两条位置指令以 `sync=1` 缓存，
再广播 `00 FF 66 6B` 触发。

### 用法

```bash
python tools/gimbal.py [选项]
```

| 选项 | 说明 |
|---|---|
| `--addrs 1,2` | 两个电机地址 |
| `--mirror 2` | 镜像安装、角度需取反的电机地址；不给则用配置里的（空串 = 都不取反） |
| `--tilt 30` | 俯仰幅度/度 |
| `--cycles 3` | 俯仰来回次数 |
| `--spin 170` | 自转角度/度（`0` = 跳过） |
| `--rpm 45` / `--acc 80` | 转速 / 加速度档位 0-255 |
| `--dwell 0` | 俯仰结束到自转之间的停顿秒数 |
| `--repeat 1` | 整体周期重复次数（**`0` = 无限循环**，Ctrl+C 停车） |
| `--once` | 只走单程 `+tilt` 并停在终点（不来回、不自转），**用于确认方向** |
| `--show` | 只读当前姿态后退出 |
| `--zero` | 把当前姿态记为 pan/tilt 零点并写回配置 |
| `--no-limits` | 本次跳过软限位检查 |
| `--config <路径>` | 指定配置文件 |

### 退出码约定（用户拍板）

| 码 | 含义 |
|---|---|
| `0` | 正常 |
| `1` | 真故障（掉线 / 未到位超时） |
| `2` | 软限位拦截 |

**`2` 不是故障**，是保护生效。但 VS Code 一样会画红字，所以 `guard()` 里额外打了一行
「保护生效，不是故障」。预设 15 的名字也带上了这个说明。

### 标定要点

- **2 号电机是镜像安装的**（2026-09-16 实测确认）：给它发同向命令，机构上得到的是反向。
  所以"世界同向"（俯仰）时**必须把 2 号的命令角度取反**。`--mirror 2` 就是干这个的（配置里默认开）。
- 确认方式：`gimbal.py --tilt 45 --rpm 15 --once` 单程停住 → 看到的应该是"俯视"。
- **注意**：`--tilt 15` 这种小幅度来回是**跑完回原点、读数不变**，看位置是查不出有没有动过的；
  要确认得用 `--once` 单程停住。
- 角度 ↔ 脉冲：3200 脉冲/圈，`脉冲 = 角度 × 3200 / 360`（30° ≈ 267）。

### 自转是「按圈换向」的

第 1 圈 `+spin`，第 2 圈 `-spin`，第 3 圈 `+spin`……

**为什么**：`pan` 是**累加**坐标（不是绕圈取模）。同向连转会一路累加出去、
被软限位永久卡死——一旦 360° 真的走完，`pan=360`，之后**每一步连俯仰都会被拦**，
云台永久卡死，只能 `--zero` 复位。

**约束**：`spin ≤ min(pan_max, -pan_min)`。开局若 `spin` 超过这个容量会打警告，
发命令前仍由 `guard()` 兜底拦截。

### 软限位

发位置命令**之前**调 `guard()` 查软限位，越界直接拒绝、不发命令：

```
[X] 软限位拦截（自转 +200.0°）：自转 200.00° 超出 [-180, 180] —— 本次不发命令。（保护生效，不是故障）
```

例外：**回零故意不过软限位闸门**——回零是找参考点的动作，否则从限位外就没法回零了。

---

## 4. `scope.py` — 实时波形示波器

**意义**：唯一的「边动边看」手段。**因为 CAN 适配器一次只能被一个程序占用，
「跑动作」和「看波形」必须在同一个进程里**，所以 `--motion` 让云台动作在这个进程的
后台线程跑，示波器在前台看曲线。

### 架构

采集线程轮询 `43 7A` → numpy 预分配环形缓冲 → Qt 定时器 33ms 刷曲线 → `setData()`。

pyqtgraph 只是把 numpy 数组的指针换给曲线、不重画整张图，所以能跑到 30FPS；
matplotlib 每次 `setData` 都要重建 artist，只做得到几帧，做实时窗口会卡。

已启用两个真示波器也在用的 pyqtgraph 内建：

- `setClipToView(True)` — 只画窗口内的点
- `setDownsampling(auto=True, method="peak")` — 峰谷抽点

### 用法

```bash
python tools/scope.py                       # 看 1、2 号，10 秒滚动窗
python tools/scope.py --hz 100 --window 5
python tools/scope.py --demo                # 假数据，不碰硬件，先看界面长啥样
python tools/scope.py --save run.csv        # 顺带落盘，之后用 plot.py 复盘

# 边动边看
python tools/scope.py --motion                              # 俯仰来回 + 自转
python tools/scope.py --motion once --tilt 45               # 单程确认方向
python tools/scope.py --motion home --home-mode 0           # 回零，看位置曲线扑向零点
python tools/scope.py --motion --repeat 0 --tilt 30 --cycles 3 --spin 170   # 无限循环
```

| 选项 | 说明 |
|---|---|
| `--hz 20` | 每台电机的轮询频率。**实测两台一起读全量 `43 7A` 也能到 ~130Hz**，放心往上开 |
| `--window 10` | 滚动窗口秒数 |
| `--secs 0` | 环形缓冲长度秒数（默认 = 窗口 × 6） |
| `--swait 0.08` | 每点等待应答秒数 |
| `--save <csv>` | 同时写 CSV |
| `--demo` | 不碰硬件，生成假数据 |
| `--motion [cycle\|once\|home]` | 同时跑哪个动作 |
| `--home-mode 0` | `home` 用哪种回零模式 |
| `--repeat 1` | 动作重复轮数（**`0` = 无限**） |
| `--tilt / --cycles / --spin / --rpm / --acc` | 动作参数，同 `gimbal.py` |
| `--no-limits` | 跳过软限位 |

### 快捷键

| 键 | 作用 |
|---|---|
| `空格` | 暂停 / 继续绘图 |
| `X` | **立即停车**（急停）+ 动作线程标记为已中止 |
| `C` | 清屏 |
| `S` | 存图 `scope_YYYYmmdd_HHMMSS.png` |
| `Q` / `Esc` | 退出（退出时自动急停、关采样线程） |

### 状态栏

示例（无限循环运行中）：

```
地址1 100.0Hz 丢0   |   俯仰+自转 进行中（第 7 轮，无限循环）   |   空格暂停  X急停  C清屏  S存图  Q退出
```

停止后会变成「已停止（共跑 41 轮，正常）」。

### 为什么"边动边看"必须同进程（线程安全设计）

「发命令 + 收应答」必须是一段**原子事务**，否则两个线程各自的 `send`/`collect`
会交错、互相把对方的应答收走。为此给 `zdt_can.Motor` 加了 `lock` + `request()`，
`cmd()` / `do_sample()` / `gimbal.status_byte()` 全走 `request()`。

`gimbal.make_ctx()` 是抽出来的公共参数组装函数，`gimbal.main` 和 `scope` 共用这一处。

> **断点拦不住电机**：调试器在 PC 端暂停时，电机照走（它是自主的）。所以发现问题**就地按 X**，
> 不要去够终端——终端可能正被断点占着。断点还会把采集线程一起冻住，
> 恢复后适配器缓冲里堆的那批过期帧会一股脑涌进来。

---

## 5. `plot.py` — 离线画图

**意义**：复盘工具。读 `zdt_can.py sample` 或 `scope.py --save` 产出的 CSV，
画**位置 / 转速 / 相电流 / 位置误差**四张子图。多个文件叠在一起就是多组实验对比。

### 用法

```bash
python tools/plot.py sample.csv
python tools/plot.py before.csv after.csv        # 叠在一起对比（不同线型区分）
python tools/plot.py sample.csv --save fig.png   # 存图不开窗
python tools/plot.py sample.csv --addr 1         # 只看 1 号
python tools/plot.py sample.csv --title "联调-第3轮"
```

`--addr` 用于从多电机 CSV 里只挑一台；`--save` 存 PNG 而不开窗（无头环境也能用）。

---

## 6. `config.py` + `gimbal_config.json` — 标定参数

**意义**：把 mirror / 运动学符号 / 零点 / 限位 / 运动默认值落盘，
`gimbal.py` 和 `scope.py` 自动读取，CLI 参数仍可临时覆盖。
**将来搬 MCU 时，这份 JSON 就是参数表的原型。**

### 用法

```bash
python tools/config.py            # 打印当前配置（缺省 + 文件覆盖后的结果）
python tools/config.py --init     # 写出缺省配置到 gimbal_config.json（已存在则不覆盖）
python tools/config.py --path     # 只打印配置文件路径
```

### 参数含义

```json
{
  "motors":      { "1": {"mirror": false}, "2": {"mirror": true} },
  "kinematics":  { "pan_sign": 1, "tilt_sign": 1, "zero_pan": 0.0, "zero_tilt": 0.0 },
  "limits":      { "tilt_min": -45.0, "tilt_max": 45.0,
                   "pan_min": -180.0, "pan_max": 180.0 },
  "motion":      { "rpm": 45, "acc": 80, "tilt": 30.0,
                   "cycles": 3, "spin": 170.0, "dwell": 0.0 },
  "pulses_per_rev": 3200
}
```

| 段 | 含义 |
|---|---|
| `motors.*.mirror` | 该电机是否镜像安装（世界转角 = −编码器角度） |
| `kinematics.*_sign` | 姿态显示的方向符号；如果是"显示反了"改这里，不用动运动学 |
| `kinematics.zero_*` | 标定零点（原始编码器坐标系），`gimbal.py --zero` 会写这里 |
| `limits.*` | 软限位，发命令前检查 |
| `motion.*` | 动作默认值，不传 CLI 参数时用这些 |

**改配置的注意**：`spin` 必须 ≤ `min(pan_max, -pan_min)`（见 3. 自转按圈换向）。

---

## 7. 两条使用姿势

### A. VS Code 预设（日常用这个）

`.vscode/launch.json` 里有 26 个预设，按 F5 选一个就行，**不用记命令行**。
编号即使用顺序：

| 编号 | 用途 |
|---|---|
| `01`–`06` | 扫描在线 / 读状态 / 使能 / 失能 / 急停 / 解除堵转 |
| `07`–`09b` | 读回零参数 / 回零标志 / 触发回零 / 退出回零 |
| `10`–`15` | 云台动作：单程确认方向 / 来回+自转 / 无限循环 / 看姿态 / 标零点 / 软限位演示 |
| `20`–`22` | 采样 10 秒 → CSV / 画波形 / 存 PNG |
| `23`–`27` | 实时波形：看两台 / 演示模式 / +云台动作 / +单程 / +回零 |
| `28` | 往返延迟对比 |
| `29` | 实时波形 + 无限循环（窗口里按 X 急停） |

**建议的上机验证顺序**：`07`（读回零参数）→ `08`（回零标志）→ `09`（触发回零）
→ `15`（软限位应被拦，退出码 2 是正常的）→ `20`（采样）→ `21`（画图）。

### B. 命令行

需要组合参数、或要写脚本自动化时用。上面每个文件的小节里都有例子。

---

## 8. 已知的坑

### `drain()` 曾给每条读取白加 15ms（2026-09-17 已修）

**症状**：`latency` 一跑，6 条命令**全部 ~15ms**，而且**跟应答长度无关**——
`36` 只 1 帧 7 字节，`43 7A` 是 5 帧 31 字节，一样 15ms。

**定位**：逐段计时发现 `drain()` 自己就 15ms，而 `request()` 每次读之前都调它。

**机理**：`drain()` 用 `bus.recv(timeout=0)`，但 python-can 的 gs_usb 后端是
`timeout_ms = round(timeout*1000) if timeout else 1`——`timeout=0` 是假值 → 变成 1ms，
而 Windows 上 libusb 的短超时会凑到系统定时器节拍（~15.6ms），
**空缓冲一次 recv 就要 15ms**。

**坑中坑**：想改成真非阻塞**做不到**——传 `timeout=1e-6` 会被 `round()` 成 `timeout_ms=0`，
而 libusb 把 0 当**无限阻塞**，直接挂死。正确做法是**别调 drain**。

**修法**：只在缓冲可能脏时才清（启动时、或上次没收全）。`collect(early=True)` 恰好收全即返回、
不留残帧，所以成功路径不需要 drain。

**修后**：`36/35/27/37/3A` 全部 ~0.7ms，`43 7A` ~1.9ms；两台轮流读全量 `43 7A` 也能到 **~130 Hz/台**。

> **推论**：之前记的"16.6 Hz 是主机问、电机答模式的天花板"**是错的**，那是这个 bug。
> `scope.py` 的 12 Hz 同理。**`11 18` 定时推送不必做了。**

**心法**：任何时候怀疑"总线/电机慢"，先用 `latency` 量，
并记住「**跟负载无关的常数延迟，先怀疑主机侧**」。

### 其他

- **`read-homing` 用功能码 `22`，不是 `AE 4B`**。手册 5.4.5 与固件 `Emm_V5_Origin_Read_Params`
  都写的是 `22`；`AE 4B <svF> <id>` 其实是**修改电机 ID/地址**。
  （逆向来的 `CAN上位机gui源码/zdt_V1/zdt_emm_v5.py` 在这一点上是错的，别信它。）
- **`--repeat 0` 前先想好怎么停**。用 `scope.py` 的话按 X/Q；用 `gimbal.py` 的话 Ctrl+C。
- **别在 VS Code 里连点多次预设**，容易踩到适配器锁死（见开头）。

---

## 9. 协议速查

> **权威来源只有两个**：① `ZDT_XS系列第二代闭环步进电机资料/3. 说明书/manual.txt`（含逐字节例子）；
> ② 官方固件源码 `.../Emm固件/.../Src/Emm_V5.c` 和 `can.c`。
> **改任何 CAN 命令字节前，先查这两处**，不要查逆向的那份 py。

### 帧格式（实测确认，逐字节核对过 `can_SendCmd`）

```
扩展帧 ID = (地址 << 8) | 包号     包号从 0 开始
数据     = 功能码 + 参数 + 校验(默认 0x6B)
```

**地址只在 CAN ID 里，不要放进数据里**（放进去电机回 `EE` 命令格式错误）。
长命令按 7 字节分包，每包开头重复功能码。

### 常用功能码

| 功能码 | 含义 |
|---|---|
| `43 7A` | 读系统状态参数（31 字节拆 5 帧） |
| `36` / `35` / `27` / `37` / `3A` | 读实时位置 / 转速 / 相电流 / 位置误差 / 状态标志 |
| `FD` | 位置模式（`FD dir vel2 acc pos4 raF snF`） |
| `F6` | 速度模式 |
| `FE 98 <snF>` | 立即停止（4 字节） |
| `FF 66` | 触发多机同步 |
| `F3 AB <state> <snF>` | 使能 |
| `0E 52` | 解除堵转 |
| `9A <mode> <snF>` | 触发回零 |
| `9C 48` | 退出回零 |
| `3B` / `3C` | 回零标志 / 回零标志+电机状态 |
| `22` | 读回零参数 |
| `93 88 <svF>` | 设置单圈零点 |
| `11 18 <func> <msH> <msL>` | 定时自动返回（**只接受 5.5 节的功能码**；`43 7A` 属 5.8，不能推） |

### 位域

**回零标志**（手册 5.4.4）：bit0 `Enc_Rdy` / bit1 `Cal_Rdy` / bit2 `Org_SF`（正在回零）/
bit3 `Org_CF`（回零失败）/ bit4 `Otp_TF` / bit5 `Ocp_TF`

**电机状态标志**（手册 5.5.15）：bit0 `Ens` 使能 / bit1 `Prf` 到位 / bit2 `Cgi` 堵转 /
bit3 `Cgp` 堵转保护 / bit4–5 左右限位 / bit7 `Oac_TF` 掉电标志

---

## 10. 现场排查的根因优先级

历史排查（2026-09-15）证明"读不到数据"的根因优先级：

1. 小屏不显示 + 蓝灯常亮 = 固件卡 bootloader → 重刷
2. 接线松紧 / 接触不良 → 重插压紧
3. 地址重复
4. `P_Serial` 必须 = `CAN1-MAP`
5. CAN_H / CAN_L 接反
6. 共地 + 120Ω 终端电阻

（用户之前在 Y42 GUI 里遇到的"地址 2 无数据返回"是 **GUI 侧问题**，不是硬件或地址问题——
`scan` 实测 1 号和 2 号都在线。）
