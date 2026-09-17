# CAN.exe 逆向分析记录

日期: 2026-09-12

## 一、二进制指纹

| 项目 | 值 |
|---|---|
| 大小 | 22.4 MB (23,530,496 B) |
| 格式 | PE32, x86 (32位), i386, GUI |
| 编译器 | MinGW-W64 GCC 7.3.0 (i686-posix-dwarf-rev0) |
| 工具链 | C++ (Qt5), 混含 Delphi (嵌入式 PE1) |
| 原始文件名 | Emm5.exe (VS_VERSION_INFO: OriginalFilename / ProductName) |
| 描述 | embeds a virtual CAN FD interface |

## 二、文档结构 (11 个区段)

| 区段 | 虚拟大小 | 虚拟地址 | 原始大小 | 说明 |
|---|---|---|---|---|
| .text | 0x4e8b4 | 0x1000 | 0x4ea00 | C++ 代码 |
| .data | 0x144 | 0x50000 | 0x200 | |
| .rdata | 0x53e78 | 0x51000 | 0x54000 | Qt 元对象、字符串 |
| .eh_fram | 0x94d0 | 0xa5000 | 0x9600 | |
| .bss | 0xdcc | 0xaf000 | 0x0 | |
| .idata | 0x5aa8 | 0xb0000 | 0x5c00 | |
| .CRT/.tls | - | - | - | |
| .rsrc | 0x4560 | 0xb8000 | 0x4600 | |
| .enigma1 | 0x1000 | 0xbd000 | 0x157a000 | Enigma 加密区 |
| .enigma2 | 0x40000 | 0xbe000 | 0x40000 | | 

→ **加壳工具: Enigma Protector** (enigma1/enigma2 标准表)。

## 三、导出的 DLL (外部依赖)

ntdll, kernel32, oleaut32, ole32, libgcc_s_dw2, user32, advapi32,
**qt5core / qt5gui / qt5network / qt5serialport / qt5widgets / qt5xml**,
msvcrt, setupapi, shell32, **winusb** (USB-CAN 设备), sxs, shfolder, shlwapi, loaderx86

加载的 Qt 插件: platforms/qwindows.dll, imageformats/*, iconengines/qsvgicon, styles, bearer,
translations qt_*.qm (英文 + zh_TW)

## 四、三个嵌入式 PE

| 偏移 | 编制 | 区段 | 说明 |
|---|---|---|---|
| 0x1630c00 | x86 | 7 | **Delphi 启动器 (CFileDescription / VirtualBox Globals 等 Delphi RTTI)** |
| 0x1665d0c | x86 | 1 | 属性 |
| 0x166630c | x64 | 1 | 属性 |

→ 外壳先运行 Delphi 启动器, 再映射主 C++ Qt 二进制 (emm5.exe)。

## 五、主程序识别出的类、文件 (Qt 元对象)

- qt5QtCore.dll
- 可见类: `MainWindow`, `CanListener`, `SLCANDriver`, `SLCANInterface`,
  `CandleApiDriver`, `CandleApiInterface`, `CANBlasterDriver`, `CANBlasterInterface`,
  `MeasurementSetup`, `ConfigurableWidget`, `GenericCanSetupPage`,
  `CanTrace`, `LogModel`, `CanMessage`
- 类标识: `MainWindow`, `CanMessage`, `CanTrace`

## 六、主界面与槽函数 (QMetaObject, 逆变版命名)

- `MainWindow` 的方法: `on_ReadSysConfs_clicked`, `on_ReadSysOriginConfs_clicked`,
  `on_ReadSysPID_clicked`, `on_ReadSysParameters_clicked`, `on_RefreshCanCard_clicked`,
  `on_ResetInterfaceParams_clicked`, `on_SerialReadData`, `on_SerialReadyRead`,
  `on_SetOrigin_clicked`, `on_WriteSysConfs_clicked`, `on_WriteSysID_clicked`,
  `on_WriteSysOriginConfs_clicked`, `on_WriteSysPID_clicked`, `on_clearCurPosition_clicked`,
  `on_disableDriver_clicked`, `on_enableDriver_clicked`, `on_exitOrigin_clicked`,
  `on_pushButtonConnect_clicked`, `on_resClogging_clicked`, `on_stopImmediately_clicked`,
  `on_synMotion_clicked`, `on_trajPosControl_clicked`, `on_trigOrigin_clicked`,
  `on_velControl_clicked`

## 七、控制标题 / 选项 (UI 字符串)

- `Bitrate:`, `CanFD Bitrate:`, `Listen only mode`, `One-Shot mode`, `Sample Point:`,
  `Triple Sampling`, `Auto-Restart on bus off condition`, `Interface:`, `Interface Details:`,
  `Options:`, `Name : %s `, `Description : %s `, `Manufacturer: %s `
- `configured by operating system`, `CANable SLCAN`, `CANable with CANFD support`,
  `CANable with standard CAN support`, `CANblaster: start listen`, `CANblaster: stop listen`,
  `Serport connect failed!`, `CANBlaster Bind Failed!`

## 八、CAN 驱动层

- `SLCANDriver/SLCANInterface` (CANable, PT-Z CANal), `CandleApiDriver/CandleApiInterface`
  (lawicel/candleLight), `CANBlasterDriver/CANBlasterInterface`
- `CAN_Write`, `GenericCanSetupPage`
- `CANblaster: start listen`, `Invalid CANblaster server. Protocol: %s  Version: %d `

## 九、具体协议相关字符串

- `command_calCRC8`, `command_calXOR`, `modbus_calCRC` (modbus RTU)
- `ReadSysConfs`, `WriteSysConfs`, `ReadSysPID`, `WriteSysPID`, `ReadSysOriginConfs`,
  `WriteSysOriginConfs`, `ReadSysParameters`
- `on_SetOrigin`, `on_trigOrigin`, `on_exitOrigin`, `on_enableDriver`, `on_disableDriver`,
  `on_clearCurPosition`, `on_stopImmediately`, `on_synMotion`, `on_resClogging`,
  `on_velControl`, `on_trajPosControl`

## 十、额外信息

- `SafetyDump` 之类字符串未见。`Emm5.exe` 文件名在版本资源中; OriginalFilename 值被 Enigma 加密/混淆。
- 界面有 CAN FD 位率设置: `CanFD Bitrate:`, `CanFD SamplePoint:`

### 待办 / 下一步
1. 动态脱壳: 在 x64dbg(x32) 中运行 CAN.exe, OEP dump 内存, 修复导入表 -> 目标输出干净 PE
2. Ghidra 恢复 C++ 符号, 再理解为源码