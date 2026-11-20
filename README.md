# miniTask

Windows x64 托盘 CPU 负荷指示器。图标竖条显示整机 CPU 使用率，默认每 500 ms 采样一次。负荷持续低于 3% 达 10 秒后改为每 2 秒采样一次；采样值达到 8% 时恢复半秒。进程以 Windows 高优先级运行。

运行 `dist/miniTask.exe`。右键单击托盘图标，可勾选打开「历史波形」、开启「记录最高负荷进程」「记录日志」以及关闭「登录时启动」，也可退出。历史波形显示最近 60 秒。首次运行会启用当前用户登录启动。

日志默认关闭，开启后写入 `%LOCALAPPDATA%\miniTask\cpu-v2.csv`，字段包括时间、整机 CPU 百分比、该采样区间 CPU 占用最高的进程及占比、已观察到的整机峰值及其时间和进程名。文件到 2 MiB 时轮转为 `cpu-v2.csv.old`。进程排名可独立关闭，关闭时日志里的进程字段为空。

使用 WinLibs x64 GCC 构建。运行 `cscript //nologo build\jscript\build.js release` 生成正式 EXE，运行 `cscript //nologo build\jscript\build.js debug` 生成带诊断消息的调试 EXE。正式版目标小于 20,000 字节，调试版目标小于 100,000 字节。

本版本在 Windows 10 x64 19045 实测。CPU 总体统计在单处理器组机器上适用，最多 64 个逻辑处理器。技术方案和验证数据见根目录的《设计方案说明》和《实施计划》。
