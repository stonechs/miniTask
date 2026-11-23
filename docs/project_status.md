# 项目状态

日期：2026-11-23。发布版本 V1.0。目标平台 Windows x86/x64。

统一发布脚本 `build/release-all.ps1` 一次构建四种交付文件：x86 普通版、x64 普通版、x86 Crinkler 版、x64 UPX 版。现有单架构调试构建仍由 `build/jscript/build.js` 提供。Crinkler x86 版需要 SSE4.2。

V1.0 验证通过：x86 普通版 12,800 B；x64 普通版 14,336 B；x86 Crinkler 版 4,426 B；x64 UPX 版 8,192 B。PE 架构检查正确，UPX 自检通过；Crinkler x86 和 UPX x64 版本均在本机启动，找到托盘窗口并正常退出。未覆盖冷启动、真实登录启动和长期负荷等场景。

构建：`powershell -ExecutionPolicy Bypass -File build/release-all.ps1`。正式发布产物附在 GitHub V1.0 Release，`dist/` 不纳入源码仓库。

2026-11-23：正式 x86 Crinkler 构建固定传入 `/HASHSIZE:1`（1 MiB）。完整发布构建通过；Crinkler 输出确认 `Hash size: 1 MB`。新版 x86 Crinkler EXE 为 5,113 B（V1.0 初始构建默认参数版为 4,426 B）。PE 架构为 x86，后台启动后优先级为 High、工作集约 8.36 MiB，托盘窗口 PID 匹配并正常退出。UPX 自检通过。构建有一条 `DefWindowProcW` forward RVA 警告，Crinkler 自动替换为 `NtdllDefWindowProc_W`，未影响构建和启动。本地构建产物已更新；未推送或改写 V1.0 远程发布。
