# CAN.exe 动态脱壳工序 (Pe-Sieve + x64dbg 路径)

摘要：
- CAN.exe 由 Enigma Protector 加壳，x86。
- 我的检查器里已内置 pe-sieve32.exe v0.4.1.1（支持对 x86 进程 dump + /imp 重建 IAT）。

## 几步总览
1. 双击运行 CAN.exe（真机或本机，保持窗口）。它需要 Qt5 运行库 & USB-CAN、串口设备可选。
2. 记下它吃掉的 PID（任务管理器->详细信息->PID，或 `Get-Process CAN | Select Id`）。
3. 以管理员身份运行 `pe-sieve32.exe /pid <PID> /imp 2 /data 4 /refl /outp <outdir>` 从运行中的进程 dump + 自动修复导入表。
   - `/imp 2` 或换 `/imp 1`(auto)。若输出里 IAT 不完整，再 `/imp 4 或 5`。
   - `/data 4` 读不可访问页（Enigma 是这么藏的）。
4. dump 出的扇区（一般叫 xxx.exe 或 .oep）即可交给 Ghidra 分析（PE32, 对齐正常）。
5. 若 dump 产物无法直接双击（缺导入），先尝试拿下来回到 x64dbg 用“Scylla”fix imports —— 但它不能当独立 app。

## 负责人提醒（IAT + 段）
- Enigma 把 IAT 藏在保护层后。pe-sieve 的 /imp 模式就是专门处理这类情况的。
- 若所有导入都是 0x 未解析，尝试在 x64dbg (F9 bypass 反调试后 F8 走到 OEP) 用 Scylla 现场扫 IAT。
- 目标导出为“干净的 exe”并不保证能直接双击？本教程目标是得到可分析的 PE，Ghidra 能识别 Qt 符号即可。

## 我这边可以做后续
- 你把 dump 出的 PE 给我（或放同一目录），我用 Ghidra 头 + pefile 复原类/槽符号、协议常量，输出成 markdown 报告。
