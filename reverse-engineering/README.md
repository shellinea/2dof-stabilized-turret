# 逆向工程：从加壳的官方上位机还原 CAN 协议

## 为什么要做

官方上位机 `ZDT_Y42_Emm_CAN_Tool` 是纯 GUI，只能点鼠标——没法脚本化、
没法一边跑动作一边采数据，也没法在 VS Code 里断点调试。

厂商手册虽然给了协议，但**实际程序用的字节格式和手册描述存在差异**
（手册给人看的和程序真正发出去的不完全一致），所以决定直接对着程序确认。

## 做了什么

目标程序 `CAN.exe`（23.5 MB，PE32 x86，**Enigma Protector 加壳**）。
直接反汇编只能看到壳代码，所以走了一条完整的还原链路：

```
加壳 CAN.exe
   │  ① Pe-Sieve 扫内存定位 OEP，x64dbg dump
   ▼
CLEAN_RECONSTRUCTED.exe（脱壳后）
   │  ② 修 IAT 与段表
   ▼
可反汇编的 PE
   │  ③ Ghidra headless 批量反编译
   ▼
Qt MOC 元对象（全部类名 / 槽函数名 / UI 字符串）
   │  ④ 从 connect() 调用点还原信号槽接线
   ▼
逐个槽函数反编译 → 拿到实际发出的命令字节
   │  ⑤ 与厂商手册交叉比对
   ▼
完整的 Emm_V5 CAN 协议 + 一份**可运行的独立实现**
```

## 目录

| 路径 | 内容 |
|---|---|
| `notes/unpack_readme.md` | ① ② 脱壳工序：Pe-Sieve + x64dbg 路径，IAT 与段表修复要点 |
| `notes/REVERSE_ENGINEERING_NOTES.md` | 二进制指纹、文档结构、嵌入 PE、识别出的类与文件、主界面槽函数 |
| `notes/UNPACKED_CAN_APP_REPORT.md` | 脱壳后程序的完整分析：应用标识、代码结构、功能参数、界面文案 |
| `ghidra_scripts/` | Ghidra headless 批量反编译脚本（`DecompileAll.java` + `decompile_all.py`） |
| `reimpl/protocol_notes.md` | **自二进制还原的协议速查**：帧结构、指令负载分类、三种校验模式 |
| `reimpl/mainwindow.h` | 重建的主窗口类声明（类名/成员名取自 MOC 字符串与 RTTI） |
| `reimpl/mainwindow_wiring.cpp` | 重建的信号槽接线（取自 `QObject::connect()` 调用点） |
| `reimpl/zdt_emm_v5.py` | 协议层：帧打包/解包/校验，含**帧级自检断言** |
| `reimpl/zdt_tool.py` | 命令行版复现 |
| `reimpl/zdt_gui.py` | 图形界面版复现（镜像原程序按钮布局） |
| `reimpl/README.md` | 逆向工程作业报告：完整证据链 |
| `reimpl/使用说明.md` | 复现版上位机的运行与操作说明 |

## 复现产物能跑

```bash
cd reimpl

python zdt_emm_v5.py      # 协议自检：对照二进制里还原出的帧逐条断言
python zdt_tool.py        # 命令行版
python zdt_gui.py         # 图形界面版（也可双击 启动上位机.bat）
```

`zdt_emm_v5.py` 的自检不依赖硬件——它把还原出的每一帧与二进制里的实际字节逐一比对，
是这套逆向结论的**可重复验证**。

## 还原出的关键结论

- **帧格式**：扩展帧 `ID = (地址 << 8) | 包号`，包号从 0 开始；
  地址**只在 CAN ID 里，不放进数据**（放进去电机会回 `EE` 命令格式错误）
- **长命令**：按 7 字节分包，每包开头重复功能码，接收端按包号重组、以校验码收尾
- **校验算法**：三种模式（固定 `0x6B` / 异或 / CRC-8），默认用固定值
- **`AE 4B` 是改电机地址，不是读回零参数**——手册 5.4.5 与固件里
  读回零参数的功能码是 `22`。这一点上一些公开的第三方实现是错的

## 与主项目的关系

还原出的协议直接支撑了 [`tools/zdt_can.py`](../tools/zdt_can.py)：
它的帧构造逐字节对照过官方固件 `Emm_V5.c` 的 `can_SendCmd`，
并与本目录的逆向结论交叉验证。

后续 ESP32 固件里的 `components/motor_can/emm_protocol.*` 会直接移植这一段逻辑，
**不需要在 MCU 侧重新逆向**。
