# 项目状态

日期：2026-11-23。发布版本 V1.1。目标平台 Windows x86/x64。

统一发布脚本 `build/release-all.ps1` 一次构建四种交付文件：x86 普通版、x64 普通版、x86 Crinkler 版、x64 UPX 版。现有单架构调试构建仍由 `build/jscript/build.js` 提供。Crinkler x86 版需要 SSE4.2。

V1.0 初始构建：x86 普通版 12,800 B；x64 普通版 14,336 B；x86 Crinkler 版 4,426 B；x64 UPX 版 8,192 B。

构建：`powershell -ExecutionPolicy Bypass -File build/release-all.ps1`。V1.0 的 Crinkler 4,426 B 文件大小保留为历史参考；`dist/` 不纳入源码仓库。

V1.1 验证通过：x86 普通版 12,800 B；x64 普通版 14,336 B；x86 Crinkler 版 5,113 B；x64 UPX 版 8,192 B。正式 x86 Crinkler 构建固定传入 `/HASHSIZE:1`（1 MiB），构建输出确认 `Hash size: 1 MB`。PE 架构检查正确，UPX 自检通过；Crinkler x86 后台启动后优先级为 High、工作集约 8.36 MiB，托盘窗口 PID 匹配并正常退出。构建有一条 `DefWindowProcW` forward RVA 警告，Crinkler 自动替换为 `NtdllDefWindowProc_W`，未影响构建和启动。

发布说明：`docs/releases/V1.1.md`。V1.1 快照位于 `dist/V1.1/`，map/obj 位于其中的 `build-support/`；`dist/` 根目录不放构建产物。构建命令：`powershell -ExecutionPolicy Bypass -File build/release-all.ps1`。发布资产包含四种架构/压缩组合。
