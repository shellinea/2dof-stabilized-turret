# UNPACKED_CAN_APP.exe — 逆向分析报告

生成时间: 2026-09-12 (来自 PID 8592 内存 dump)

## 0. 来源说明
- 文件 `400000.CAN.exe` 由 pe-sieve (v0.4.1.1, `/imp` 重建导入) 从运行的 CAN.exe (Enigma Protector 加壳) 中抓取。
- 本质是 **Emm5.exe** (`张大头闭环伺服 ZDT_Y42_Emm_CAN_Tool_V1.2.4`)。
- 平台: Win32 x86, MinGW-W64 GCC 7.3, Qt5 (widgets/serialport/network/xml/svg)。

## 1. 应用标识
- 主窗口标题: `张大头闭环伺服 ZDT_Y42_Emm_CAN_Tool_V1.2.4`
- 这是「张大头」(ZDT) 品牌**闭环伺服电机**上位机,走 CAN 总线控制电机(原点回零、运动、PID、检测堵转等)。

## 2. 代码结构 (Qt MetaObject 全部类)
```
MainWindow                    (QMainWindow) — 主界面,全部 on_ 槽
  ├ 连接CAN / CAN 端口设置: on_RefreshCanCard_clicked, on_pushButtonConnect_clicked,
  │   SerialPort(QSerialPort), openSerialPort_8/9, on_SerialReadyRead, on_SerialReadData
  ├ 读/写 系统参数: on_ReadSysParameters_clicked, on_WriteSysConfs_clicked, on_ReadSysConfs_clicked, on_WriteSysID_clicked, on_ReadSysPID_clicked, on_WriteSysPID_clicked
  ├ 原点回零: on_ReadSysOriginConfs_clicked, on_WriteSysOriginConfs_clicked, on_SetOrigin_clicked, on_trigOrigin_clicked, on_exitOrigin_clicked
  ├ 驱动板: on_enableDriver_clicked, on_disableDriver_clicked
  ├ 位置/速度控制: on_clearCurPosition_clicked, on_stopImmediately_clicked, on_synMotion_clicked, on_resClogging_clicked, on_velControl_clicked, on_trajPosControl_clicked
  └ 更新UI: updateUI() (和 QTimer timeout)
CanListener          (QObject背景线程) — 监听 CAN bus: run(), startThread(), requestStop(), waitFinish(), messageReceived(CanMessage)
CanMessage           (QObject数据) — msg
MeasurementSetup     — 测量配置: driver/interface/bitrate/configure/500000/sample-point 875/can-fd 等 workspace 配置, CanDB (.dbc) 加载
ConfigurableWidget / GenericCanSetupPage — 驱动选择页 (Driver:/Interface: 下拉)
CanDriver            — 抽象驱动
  ├ CanInterface
  ├ SLCANDriver / SLCANInterface       (CANable; "CANable 1.0..USB CDC", "CANable 2.0")
  ├ CandleApiDriver / CandleApiInterface (USB-winusb candleAPI, CANFD)
  └ CANBlasterDriver / CANBlasterInterface — 局域网 UDP 239.255.43.21 多播 ("CANblaster: start listen")
LogModel / CanTrace  — 日志模型 (Time/Level/Message), 时间格式 hh:mm:ss, _0x%08X / 0x%03X ID 显示
```

## 3. 主要功能参数 (界面说明)
- 原点回零: 回零模式(Nearest/Dir/Senless/Endstop/AbsZero/PowerCut)、方向(CW/CCW)、
  速度(RPM 0-5000)、超时(ms 0-999999)、无限位回零检测时间/电流/转速、上电自动触发回零、
  是否存储、触发回零、退出回零。单圈(就近/方向)/多圈(无限位) 两种。
- 运动控制: 位置模式(相对/绝对 0/1/2)、发送指令、目标位置角度、实时转速、
  速度/方向/加速度、脉冲数;速度模式;立即停止;同步运动(多机同步);清零位置角度。
- 读取系统状态: 总线电压(mV)、相电流(Ma)、编码器线性值、目标位置角度、实时位置角度、
  实时转速、正在回零标志、回零失败标志、使能状态标志、电机到位标志、电机堵转标志、堵转保护标志、位置角度误差。
- 驱动参数: En脚有效电平(L/H)、脉冲控制模式、电机类型(0.9/1.8/OPEN/FOC/...)、
  通讯端口复用(UART/CAN)、Dir脚正方向、细分、细分插补、自动熄屏、开环工作电流、
  堵转最大电流、最大输出电压、串口波特率(9600..921600)、CAN速率(10000..1000000)、
  通讯校验方式(0x6B/XOR/CRC-8)、位置到达窗口、堵转检测转速/时间/电流、堵转保护功能(None/Receive/Reached/Both/Other)。
- PID: Kp/Ki/Kd 读写。
- 其它: 控制命令应答、读取驱动参数、修改驱动ID地址/写入并保存、config.ini(`CanConfig`: canEn/canChannel/canDevice/canBaud/canAddr/AnswerTimeOut_CAN)。

## 4. 用户反馈文案
读取参数成功/失败 (绿/红), 指令下发成功, 返回数据错误, 写入参数成功, 电机到位完成,
指令格式错误, 指令条件不符, 没有数据返回, 关闭CAN, 未连接CAN, 连接CAN成功, 无法连接CAN!

## 5. 需要注意
- IAT 恢复到了基本可分析程度;静态反编译建议把该 exe 丢进 Ghidra(32-bit,基址0x00400000)。
- 但完整版 "源码还原" 需要先对模块内存重建 (Qt MOC + 各 DLL 动态内存,已 dump 在
  `tmp/candump/process_8592/` 下,含 Qt5*.dll)。当前文档已足够交作业用。