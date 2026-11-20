# glint 源码设计分析

分析日期：2026-11-20  
上游地址：https://github.com/Cyronius/glint  
本地只读克隆：`upstream/glint`  
克隆提交：`ef99b9cf1d30ad95734b4a0519842f9c0f075d5d`（当前 `main`）

## 结论摘要

glint 与本项目的交互构想非常相近：托盘图标用动态条形显示负荷，右键菜单勾选项目，设置可持久化，程序不需要一直显示主窗口。但它是多指标工具，带有小型浮动窗口、多个 Windows 子系统/硬件指标、Rust 标准库和 Windows crate；不是极限小内存/小文件体积样板。

它的可借鉴重点是架构边界和“只在值变化时更新图标”的优化。它的实测数据也说明系统繁忙时采样 API 的耗时可能从毫秒跃升到数百毫秒乃至秒级，因此应隔离采样线程，并区分采样完成和 UI 响应。glint 未实现本项目要求的进程 CPU 排名和负荷日志；源代码中也没有设置高进程优先级的调用。

## 架构

- `src/main.rs:40-50`：单实例互斥锁，进入安装提示（发布版）后启动 UI。
- `src/window.rs:125-219`：创建隐藏 Win32 窗口、托盘图标、设置对象，并启动独立 sampler；窗口主线程运行 Win32 消息循环（`src/window.rs:822-832`）。
- `src/sampler.rs:214-265`：Sampler 持有一个 PDH 查询和需要的指标计数器；一次 query 收集多个指标。
- `src/pdh.rs:1-7,49-90`：对 PDH 的轻量封装，原则是 query/counter 初始化一次、稳态采样复用缓存；计数器第一次 collect 仅建立速率基线。
- 采样线程把样本通过 channel 送到 UI，再以 `PostMessage(WM_SAMPLE)` 唤醒窗口线程；耗时系统调用不会卡住托盘菜单/UI（`src/window.rs:165-177`、`src/sampler.rs:543-590`）。

## CPU 采样方式

- glint 总体 CPU 并非用 `GetSystemTimes`，而是读取 PDH 计数器 `\\Processor Information(_Total)\\% Processor Time`（`src/sampler.rs:21-25,268-275,325-355`）。
- 其规范文档把 `% Processor Utility` 作为 CPU 主读数，目标是匹配任务管理器并考虑睿频影响；实现还读取 `% Processor Performance` 计算频率/boost 信息（`specs/glint/spec.md:630-645`、`src/sampler.rs:346-355`）。因此具体 CPU 计数口径有版本演进，不能仅从最初介绍推断。
- 进程排名不在其范围内。规范的 out-of-scope 说明指出逐进程列表不属于 glint（`specs/glint/spec.md:1-15`）；源码也未发现读取进程时间或枚举进程的路径。
- 未发现采样日志文件功能。配置文件仅保留少量显示选项和窗口位置（`src/config.rs:1-18,50-123`），不记录历史采样序列；内存中的 60 点历史仅用于小窗图表（`src/sampler.rs:15-76`）。

## 定时、线程与优先级

- UI 窗口本身以消息循环等待消息；采样器在独立线程使用 `recv_timeout(period)` 阻塞到采样周期或控制消息到达，不是忙轮询（`src/sampler.rs:543-590`）。
- 浮动窗口可见时采样周期 1 秒，隐藏时 2 秒；显示/隐藏时向 sampler 发送周期变更（`src/window.rs:63-66,337-358`）。这与本项目希望的高负荷 0.5 秒、低负荷 1–2 秒相近，但并未依据 CPU 负荷自适应。
- 采样线程创建后未调用 `SetThreadPriority`；源代码中也未调用 `SetPriorityClass`。因此不能认为 glint 具备本项目要求的 `HIGH_PRIORITY_CLASS`。
- 睡眠唤醒后重新创建 PDH query，避免计数器冻结（`src/window.rs:966-972`、`src/sampler.rs:284-297`）。

## 托盘图标、菜单与设置

- `src/trayicon.rs:212-253`：按每项指标生成一条横向 bar，按当前值填充；没有勾选指标时退回静态 logo。图标根据可用指标动态绘制。
- `src/window.rs:310-332`：更新 tooltip 和图标；图标变化后销毁旧 icon，避免 GDI/USER 句柄持续增长。
- `src/window.rs:640-717`：原生 Win32 popup menu，每个显示指标都有 checked 状态，并有启动、隐藏、重置位置和退出项。
- `src/window.rs:721-768`：切换显示后保存配置并更新托盘图标；自启动开关写入当前用户 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`（`src/window.rs:1053-1105`）。
- 配置位于 `%APPDATA%\\Glint\\config.json`，手写简单的扁平 JSON 读取/写入，避免额外序列化 crate（`src/config.rs:1-6,50-123`）。

## 资源优化和实际开销

已记录的上游实测（不是本任务在本机重新测得）：

- README 报告 Release EXE 约 200 KB，私有内存约 6.2 MB；隐藏窗口下 CPU 约为一个逻辑核心的 0.05%–0.4%（`README.md:25-27`）。
- 规范在 2026-10-16 的一个 24 逻辑 CPU 系统上记录：隐藏、2 秒采样、所有指标开启时，约 6.50–6.69 MB 私有字节、0.365%/0.520% 一个核心、5 个线程（`specs/glint/spec.md:183-194`）。环境有后台活动，规范明确指出读数存在噪声。
- 每个采样 tick 的 steady-state 数据缓冲复用，目标为零分配（`src/pdh.rs:1-7`、`src/sampler.rs:405-407`）；低速变化的数据（磁盘空间、网卡列表）并非每 tick 枚举（`src/sampler.rs:18-28`）。
- 图标只在条形像素长度变化时才重建/调用 `NIM_MODIFY`；规范记录稳定读数约 0.052% 一个核心，读数变化时约 0.31%–0.44%，重建图标加通知约 4 ms（`specs/glint/spec.md:267-288`）。
- 隐藏窗口不进行布局和绘制（`src/window.rs:585-610`），但仍定时采样并更新 tray 图标。
- 在繁忙负载下，规范记录 sampler tick 中位耗时从空闲 8.6 ms 增至 41 ms；网卡表刷新偶发超过 1 秒（`specs/glint/spec.md:1214-1219`）。这解释了为什么采样线程独立于消息/UI 线程。

上述结果在约 2 秒周期和多指标开启条件下测得；我们的 0.5 秒刷新、更高优先级和更少指标都会改变结果，不能简单按比例外推。100 KB 私有内存远低于 glint 的数 MB 基线；要达到该目标，需先构建一个只含托盘、计时器、总体 CPU 查询的最小原生 x64 原型，再加可选模块逐项测量。

## 对本项目的直接借鉴

1. 保持 UI 消息线程短小；采样工作可独立运行，避免在右键菜单/托盘回调中做耗时枚举。
2. 使用绝对简化的 sampler：只维护总体 CPU 指标；把逐进程枚举和文件日志作为可关闭的附加工作。
3. 缓存图标状态签名，只有显示像素确实变化时才重建 HICON 并调用 `NIM_MODIFY`。
4. 避免 glint 的浮动图表窗、多指标、GPU/NPU/网卡/磁盘/温度发现等范围，以控制私有内存和依赖面。
5. 保持高优先级要求时应让线程绝大多数时间阻塞等待，缩短唤醒工作；实际系统交互和 CPU 占用需要单独测量。
6. 若进程排名/日志开启导致开销超过目标，应允许菜单关闭它们；总体 CPU 图标采样不受影响。

## 证据边界

本分析基于当前克隆的 `main` 提交和仓库随附的 spec/README。没有构建或运行上游程序，也没有独立复测其资源数字；上游 spec 中的数值按其测试条件转述。行号对应克隆提交 `ef99b9cf1d30ad95734b4a0519842f9c0f075d5d`。
