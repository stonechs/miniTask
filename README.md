# miniTask

V1.1

Windows x64 托盘 CPU 负荷指示器。图标竖条显示整机 CPU 使用率，默认每 500 ms 采样一次。负荷持续低于 3% 达 10 秒后改为每 2 秒采样一次；采样值达到 8% 时恢复半秒。进程以 Windows 高优先级运行。

运行 `dist/V1.1/miniTask-x64.exe`。右键单击托盘图标，可勾选打开「历史波形」、切换「记录日志」和「登录时启动」，还可选择「打开日志目录」或「退出」。菜单在鼠标附近弹出。历史波形显示最近 60 秒。首次运行会启用当前用户登录启动。

运行 `powershell -ExecutionPolicy Bypass -File build\release-all.ps1` 可在 `dist/V1.1/` 生成四种发布文件，map 和 obj 放在该版本的 `build-support/` 子目录。x86 普通版使用 Clang `-Oz`，x64 普通版使用 GCC `-Os`；Crinkler 版为 x86，UPX 版为 x64。脚本默认使用本机现有 Crinkler 3.0b、UPX 5.3.0、Clang 和 Windows SDK/MSVC x86 库，也可通过脚本参数指定 GCC、Clang、Crinkler 和 UPX 路径。

日志默认开启，写入 `%LOCALAPPDATA%\miniTask\cpu-v2.csv`，首次有效采样即落盘，之后约每 2 秒写盘一次。字段包括时间、整机 CPU 百分比、该采样区间 CPU 占用最高的进程及占比、已观察到的整机峰值及其时间和进程名。文件到 2 MiB 时轮转为 `cpu-v2.csv.old`。进程排名随 CPU 采样自动运行，悬停托盘图标可查看整机 CPU 百分比和当前采样区间最高占用进程及占比；低负荷时随 CPU 采样降至每 2 秒一次。旧版本升级首次运行会开启日志，之后按右键菜单设置保存开关状态。

统一发布包使用 `powershell -ExecutionPolicy Bypass -File build\release-all.ps1` 构建四种架构/压缩组合。脚本需能找到 x64 GCC、Clang/clang-cl、Crinkler、UPX、Windows SDK 和 MSVC x86 库；工具路径可按脚本参数覆盖。x86 Crinkler 版由 Crinkler 3.0b 生成，正式构建固定使用 `/HASHSIZE:1`（1 MiB），要求支持 SSE4.2 的处理器和 Windows 10 或更新版本。单独构建 x64 未压缩版仍可运行 `cscript //nologo build\jscript\build.js release`；运行 `cscript //nologo build\jscript\build.js debug` 可生成带诊断消息的调试版。

本版本在 Windows 10 x64 19045 实测。CPU 总体统计在单处理器组机器上适用，最多 64 个逻辑处理器。技术方案和验证数据见根目录的《设计方案说明》和《实施计划》。
