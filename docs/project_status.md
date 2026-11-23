# 项目状态

日期：2026-11-23。Windows x64，WinLibs GCC 16.2.0、Clang 23.1.5、Crinkler 3.0b、UPX 5.3.0。

新增统一发布脚本 `build/release-all.ps1`，生成 x86 普通版、x64 普通版、x86 Crinkler 版和 x64 UPX 版。旧的 `build/jscript/build.js` 仍用于单独构建 x64 普通版或调试版。Crinkler x86 版要求 SSE4.2。

验证：四个产物构建成功，大小依次为 12,800 B、14,336 B、4,426 B、8,192 B。PE 检查确认 x86 两版为 i386，x64 两版为 x86-64；UPX 自检通过。Crinkler x86 和 UPX x64 版本均在本机成功启动，找到托盘窗口并正常退出。未覆盖冷启动、登录启动等长期系统场景。所有版本使用同一源文件，未定义 `MINITASK_DIAGNOSTICS`。

下一步：发布时运行 `powershell -ExecutionPolicy Bypass -File build/release-all.ps1`。
