# Windows 10 私有内存实测

Windows 10 x64，版本 19045。探针用 GCC 16.2.0 构建，无 CRT，PE 只直接导入 KERNEL32。每档启动三个新进程，启动约 800 ms 后从外部读 `PrivateMemorySize64`，每档累加上一步的操作。单位均为 KiB。

| 最后执行的操作 | 三次读数 | 中位数 | 与上一档之差 |
| --- | --- | ---: | ---: |
| 只直接导入 KERNEL32，进入 Sleep | 444 / 396 / 400 | 400 | |
| 加载 USER32 | 736 / 752 / 772 | 752 | +352 |
| 加载 GDI32 | 728 / 760 / 720 | 728 | −24 |
| 加载 SHELL32 | 844 / 860 / 852 | 852 | +124 |
| 注册窗口类、消息并创建隐藏窗口 | 1128 / 1132 / 1124 | 1128 | +276 |
| 创建 16 × 16 图标并释放临时位图 | 1164 / 1160 / 1128 | 1160 | +32 |
| 注册托盘图标并设置通知版本 | 1384 / 1392 / 1388 | 1388 | +228 |
| 第一次替换托盘图标 | 1388 / 1388 / 1392 | 1388 | 0 |

增量包含相关系统 DLL 的依赖和初始化。GDI32 两档的范围有重叠，未分辨出显著的独立增量。最后一档只有一次替换，用来观察首次调用成本。

## 正在运行的原型

对本机当时已在运行的 miniTask 调试版做了一次 VMMap 3.4 快照。直接读取 `K32GetProcessMemoryInfo.PrivateUsage` 得到 2,000 KiB，与同一时点的 `PrivateMemorySize64` 相同。VMMap 导出中，顶层区域按类别合计的 `PrivateBytes` 如下。

| VMMap 类别 | KiB |
| --- | ---: |
| Heap (Private Data) | 552 |
| Image (ASLR) | 484 |
| Thread Stack | 84 |
| Private Data | 64 |
| 区域合计 | 1184 |

VMMap 区域合计与进程计数器并未对齐，差额尚未归因。正在运行的原型与短测探针也处于不同运行阶段，不能用两者之差估算单项开销。

结论适用于这台机器和当前测试构建。空进程已经超过 100 KB。窗口和通知区初始化可见数百 KiB 的增量，1 KiB 图标像素缓冲不是主要来源。

## 复现

在仓库根目录运行 `powershell -NoProfile -File tools\measure-private-memory.ps1`。探针源码为 [memory-probe.c](../tools/memory-probe.c)。VMMap 快照使用 `vmmap64.exe -p <PID> output.mmp` 导出，只汇总顶层区域以避免重复计算子区域。[VMMap 官方说明](https://learn.microsoft.com/en-us/sysinternals/downloads/vmmap)
