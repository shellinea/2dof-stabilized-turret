# 实测数据

这里放的是**实机**采到的数据，不是仿真，也不是估算。

## 文件

| 文件 | 来源 | 内容 |
|---|---|---|
| `sample.csv` | `tools/zdt_can.py sample` | 静止基线：两台电机使能但未动作，10 秒 |
| `run.csv` | `tools/scope.py --motion` | 边跑动作边采样（俯仰 + 自转） |
| `sample.png` | `tools/plot.py` | `sample.csv` 的四张子图 |
| `make_figures.py` | — | 生成 README 配图（`docs/assets/`），不依赖硬件 |
| `esp32_can_phases.md` | ESP32-S3 自检固件 + M2e 上板 | CAN 相位 1~7 的判据与实测数字、M2e 串口关键行、`D1 07` 死命令的穷尽记录 |

## CSV 格式

前 3 行是 `#` 开头的元信息（时间、电机地址、目标频率），之后是标准 CSV。

```
t_s, addr, bus_mv, phase_ma, encoder, target_deg,
speed_rpm, pos_deg, err_deg, homing_flags, motor_status
```

| 列 | 单位 | 说明 |
|---|---|---|
| `t_s` | 秒 | 相对采样开始的时刻 |
| `addr` | — | 电机地址（1 / 2） |
| `bus_mv` | 毫伏 | **总线电压**（V+ 过反接二极管后）。12 V 供电实测约 11.0–11.2 V |
| `phase_ma` | 毫安 | 相电流。静止约 12–16 mA，堵转时会显著上升 |
| `encoder` | 计数 | 原始编码器值（3200 计数/圈） |
| `target_deg` | 度 | 目标位置 |
| `speed_rpm` | RPM | **带符号**实时转速 |
| `pos_deg` | 度 | 实时位置 |
| `err_deg` | 度 | 位置误差（`target_deg − pos_deg`） |
| `homing_flags` | 位域 | 回零标志，见下 |
| `motor_status` | 位域 | 电机状态标志，见下 |

### `homing_flags`（手册 5.4.4）

| 位 | 名称 | 含义 |
|---|---|---|
| bit0 | `Enc_Rdy` | 编码器就绪 |
| bit1 | `Cal_Rdy` | 标定就绪 |
| bit2 | `Org_SF` | 正在回零 |
| bit3 | `Org_CF` | **回零失败** |
| bit4 | `Otp_TF` | 过温 |
| bit5 | `Ocp_TF` | 过流 |

> 从未做过回零时该值实测为 `0x0B`（含 bit3 回零失败）。
> **回零是否成功有个现成判据**：跑 `9A` 前后读 `3B`，应从 `0x0B` 变 `0x03`。

### `motor_status`（手册 5.5.15）

| 位 | 名称 | 含义 |
|---|---|---|
| bit0 | `Ens` | 使能 |
| bit1 | `Prf` | 到位 |
| bit2 | `Cgi` | 堵转 |
| bit3 | `Cgp` | 堵转保护 |
| bit4/5 | — | 左 / 右限位 |
| bit7 | `Oac_TF` | 掉电标志 |

## 静止基线（`sample.csv`）

两台使能但未动作时的数据：

| 指标 | 实测值 |
|---|---|
| 位置 | 漂移 ±0.03° |
| 转速 | 恒 0 |
| 相电流 | 12–16 mA |
| 总线电压 | 11.0–11.2 V（12 V 供电） |
| `motor_status` | `0x03`（使能 + 到位） |
| `homing_flags` | `0x0B`（含 bit3 回零失败） |

这组数字是后续所有实验的对照基准——**任何异常都应该拿它比一比**。

## 复现

```bash
# 1) 重新采一份静止基线
python tools/zdt_can.py --addr 1,2 sample --secs 10 --hz 20 --log sample.csv

# 2) 边跑动作边采
python tools/scope.py --motion --tilt 30 --cycles 3 --spin 170 \
                      --hz 100 --save run.csv

# 3) 画图
python tools/plot.py sample.csv
python tools/plot.py before.csv after.csv        # 多组叠图对比

# 4) 重新生成 README 配图（不需要硬件）
python measurements/make_figures.py
```

> 采样和跑动作**必须在同一个进程里**——CAN 适配器一次只能被一个程序占用。
> 所以 `scope.py` 把动作放在后台线程，`zdt_can.py sample` 则只能单纯采样。
