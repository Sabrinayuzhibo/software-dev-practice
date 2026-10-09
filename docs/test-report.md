# 软件测试报告（草稿）

## 2026-10-09 弹窗玻璃首帧与字体授权

执行者：Codex 自动化运行。核对 Apple 开发者字体下载页 `https://developer.apple.com/fonts/` 的 SF 字体许可：下载版仅可用于指定 Apple 平台产品的界面稿，不允许嵌入应用或用于非 Apple 系统产品；没有安装或分发苹果字体。当前项目已经分发 SIL OFL 1.1 的 Noto Sans SC，它在本机实际承担中文绘制，但与苹方不可能做到字形完全相同。

在隔离的 WSLg / SwiftShader Electron 窗口，用 `/tmp/backup-sheet-perf.mjs` 记录点击“新建任务”到弹窗 `data-glass-state=active` 的耗时，并用 CPU profile 和 WebGL API 时间定位耗时。修改前连续 3 次为 835、823、826 ms；`gl.getError()` 在大弹窗绘制后同步等待约 420–450 ms，按钮渲染器在同一图形队列上也会产生额外等待。移除逐帧同步错误轮询后，保留异常捕获和上下文丢失恢复；弹窗等待首帧期间改用带色调和模糊的 CSS 玻璃回退，WebGL 激活后切换至原材质。未降低弹窗或按钮画布分辨率。

| 验证 / 命令 | 实际结果 |
| --- | --- |
| `node /tmp/backup-sheet-perf.mjs` | 修改后 3 次为 343、195、161 ms；弹窗内部画布保持 782×620，测量环境与修改前相同。单次数据受 SwiftShader 调度影响，不代表其他机器保证值 |
| `./scripts/build.sh` | Meson 13/13 通过；TypeScript 和 Vite 构建通过 |
| `node /tmp/backup-control-check.mjs` | Playwright 实窗点击、键盘、拖动、窄窗口、DPR2、上下文恢复与截图通过；中文标题仍由打包的 Noto Sans SC 绘制 |
| `node tests/desktop_window_smoke.mjs` | 第二次完整回归通过，含新增的“玻璃控件不调用同步 `getError`”、弹窗回退材质及原有像素、表单状态和上下文恢复检查。第一次在既有的开关模拟点击断言处失败，复测通过，原因未定位 |

截图 `/tmp/backup-controls-dialog.png`、`/tmp/backup-controls-narrow.png`、`/tmp/backup-controls-dpr2.png` 已目视检查：玻璃非空，标题、控件和底部操作区无重叠。未在 macOS、Windows 或硬件 GPU 上验收；不应把本机性能数据外推到其他环境。

## 2026-10-09 简体中文字体

执行者：Codex 自动化运行。根据 Apple 官方支持页面 `https://support.apple.com/en-us/103203` 的简体中文 CSS 字体栈，优先使用系统 `SF Pro SC`、`PingFang SC`。当前 WSL 没有这两种字体，因此将 Noto Sans SC 可变字库转换为 WOFF2（约 7.8 MB）随应用分发，并附带 SIL Open Font License 1.1。苹果字体文件没有纳入项目；macOS 有对应系统字体时可直接使用，WSL 使用后备字体。

| 验证 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件通过，0 失败、0 跳过；TypeScript 与 Vite 构建通过，WOFF2 与许可证均进入桌面构建 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 node /tmp/backup-control-check.mjs` | 真实 Electron + Chromium 字体诊断确认“备份任务”标题使用自定义加载的 Noto Sans SC（4 个字形）；1100/850 px、DPR=2 截图及玻璃控件交互通过 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 npm run test:window --prefix apps/backup-desktop` | 完整桌面回归通过：中文文件名、长路径、单文件/多文件、类型筛选、扫描、备份、还原、记录和窗口恢复；850 px 下表格与弹窗无横向溢出 |
| 格式 | 改动的 CSS/MJS 经 Prettier 检查通过，`git diff --check` 通过 |

Apple 字体只在已安装的系统上显示。本机检查证实 Noto 后备字体生效，未在 macOS 上验收 `SF Pro SC`/`PingFang SC` 的实际字形。截图为 `/tmp/backup-controls-{desktop,dialog,narrow,dpr2}.png`、`/tmp/backup-records-grouped-narrow.png` 与 `/tmp/backup-type-filter-folder-composer-850.png`。

## 2026-10-09 玻璃输入框、弹窗与数据页面

执行者：Codex 自动化运行，遵循 huawei-coding。沿用下文记录的上游固定提交，导入原版 Text Input 材质，并从 Dialog 构建器提取原版圆角、折射、模糊、高光和遮罩参数。原生 DOM 继续管理输入、下拉菜单、表单校验、焦点和对话框；任务卡片、表格、状态标签、折叠明细、进度及滚动条使用配套 CSS。未增加演示项目中的无关页面。

动画控件、静态输入框和弹窗使用独立的共享渲染器，避免大弹窗扩大开关每帧使用的缓冲区。按钮、开关和输入框使用本地纯色背景，弹窗使用上游壁纸与遮罩；玻璃不采样背后的实时 DOM 内容。模态框关闭后释放对应渲染器。

| 验证 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件全部通过，0 失败、0 跳过；TypeScript 检查和 Vite 构建通过。之后的前端细节修正由桌面测试启动脚本重新构建 |
| `tests/desktop_glass_fields.mjs` | 原生键盘输入、弹窗中心与透明圆角像素检查通过；同时注入输入框及弹窗上下文丢失，恢复期间仍可输入，恢复后内容、焦点和动画控件的原上下文保持不变；Escape 关闭、资源释放及重新打开通过 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 npm run test:window --prefix apps/backup-desktop` | 最终完整回归通过，覆盖扫描、备份、还原、异常反馈、历史分页、单文件/多文件/文件夹、长路径、类型筛选与特殊节点；1120px/850px 布局、5 次最大化/还原及最小化恢复通过。文件选择器返回值由测试注入，未人工操作系统选择窗口 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 node /tmp/backup-control-check.mjs` | Playwright 连接真实 Electron；1120×760、850×600、DPR=2 截图与按钮/开关交互通过。目标表单的只读/禁用状态保持；进度条两次截图不同，“减少动态效果”下停止动画；未出现页面异常或 WebGL 绘制错误 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 node /tmp/backup-control-performance.mjs /tmp/backup-glass-surfaces-performance.json` | 大玻璃弹窗打开时，DPR=1 / DPR=2 的实际控件更新为 60.0 / 59.4 FPS，P95 帧间隔 17.4 / 18.4 ms；每帧只复制当前控件，动画结束后的 450 ms 观察期为 0 次控件绘制。DPR=1 的动画缓冲仍为 141×58 CSS 像素 |
| 来源与格式 | 着色器与上游逐文件比较一致，新增输入框和弹窗来源已记录到 NOTICE 并同步到分发副本；涉及的自有 TS/TSX/CSS/MJS 通过 Prettier，`git diff --check` 通过 |

完整桌面回归发现并修复两处画布边缘导致的横向溢出：目标表单增加边缘留白，弹窗的显示画布裁切到实际内容边界，不再把临时渲染余量计入滚动宽度。记录状态列加宽，避免“完成并有警告”末字单独换行。新建任务的内容区独立滚动，底部动作区保持可见。

中间一轮在既有的“重载后失联状态”检查中出现一次 10 秒超时；业务轮询逻辑、等待时间和原断言均未调整，后续复测通过。该偶发超时原因尚未定位，不能将单次复测通过视为已修复。

证据：`build/meson-logs/testlog.txt`、`tests/desktop_glass_fields.mjs`、`/tmp/backup-glass-surfaces-performance.json`；截图包括 `/tmp/backup-controls-dialog.png`、`/tmp/backup-controls-narrow.png`、`/tmp/backup-controls-dpr2.png`、`/tmp/backup-glass-targets-dpr2.png`。性能与截图来自本机隔离的 Xvfb / Openbox、SwiftShader 环境；未修改用户正在运行的窗口或备份数据。没有验收硬件 GPU、跨真实显示器切换、触摸屏及 Windows/macOS，帧率不代表其他机器的保证。

## 2026-10-09 玻璃控件拖动性能

执行者：Codex 自动化运行。环境为 Ubuntu 24.04 / WSL2、Electron 44.4.5、SwiftShader、隔离的 Xvfb / Openbox 60Hz 显示会话。保留上游着色器和按压/释放形变；改为共用控件尺寸的临时渲染缓冲，只重画并复制有变化的控件，其他显示画布保留原像素。拖动期间旋钮位置直接跟随指针，松手后的吸附和形变仍使用上游弹簧。

在同一 1120×760 新建任务窗口、16 个已注册控件、相同循环拖动路径上比较。每个 DPR 连续拖动约 4.5 秒，绘制间隔统计排除前 0.5 秒；帧率来自被拖控件实际更新画布的时间戳，不是单独的 rAF 调用次数。

| 指标 | 修改前 | 修改后 |
| --- | --- | --- |
| DPR=1 的实际绘制帧率 | 28.4 FPS | 60.0 FPS |
| DPR=2 的实际绘制帧率 | 13.8 FPS | 60.0 FPS |
| DPR=1 / DPR=2 的 P95 帧间隔 | 37.7 / 79.0 ms | 17.4 / 17.4 ms |
| 拖动时每帧复制控件数 | 16 | 1 |
| 拖动时每帧 WebGL 绘制调用 | 约 73 | 约 9 |
| DPR=1 临时画布尺寸 | 512×226 | 141×58 |
| 动画结束后的 450 ms 观察期 | 未测 | 0 次控件绘制 |

| 验证 / 命令 | 实际结果 |
| --- | --- |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 node /tmp/backup-control-performance.mjs /tmp/backup-glass-after.json` | 上表最终结果；修改前数据保存在 `/tmp/backup-glass-before.json`。未降低 DPR、关闭玻璃效果或更换渲染驱动 |
| `tests/desktop_control_motion.mjs` | 通过真实 Chromium 鼠标事件快速反向拖动，已绘制旋钮位置与指针位移相符；拖动期间只有当前控件更新，其他控件保持原图像。由 `desktop_controls.mjs` 调用 |
| `./scripts/build.sh` | 13 个 Meson 套件全部通过，0 失败、0 跳过；TypeScript 检查与 Vite 构建通过 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 npm run test:window --prefix apps/backup-desktop` | 完整桌面回归通过，包括开关点击/键盘/拖动/禁用/图形恢复，以及扫描、备份、还原、多来源、特殊类型、历史、窗口恢复和关闭 |
| Playwright 视觉及恢复检查 | 1120×760、850×600、DPR=2 截图复核通过；图形上下文替换、页面切换和表单状态保持通过。修复恢复后尺寸尚未就绪就开始绘制的问题，复测无 WebGL 绘制错误 |
| 格式与来源 | 涉及的 TS/MJS 通过 Prettier 检查，`git diff --check` 通过；着色器与上游一致，NOTICE 已记录适配变化并同步到分发副本 |

性能测试使用真实鼠标按下和 rAF 驱动的重复指针移动，减少 CDP 往返对采样的影响；真实输入功能检查另行执行。这里的 60 FPS 是本机隔离环境下的结果，不代表其他机器、硬件 GPU、真实显示器或触摸屏的性能承诺。测试未修改用户正在运行的应用或数据。

## 2026-10-09 原版玻璃按钮与开关

执行者：Codex 自动化运行。沿用下节所列上游提交的渲染器，新增原版 Settings Toggle 的轨道、旋钮和动画；页面、弹窗、标题栏按钮使用 Surface / Tinted Blue 参数，危险操作用红色。控件保留原生 DOM 点击、表单、禁用和键盘语义，共用一个离屏 WebGL 上下文。自有代码遵循 huawei-coding；上游代码保留原格式和许可。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件全部通过，0 失败、0 跳过；TypeScript 检查与 Vite 构建通过，控件集成后无大包警告。后续前端调整由桌面启动脚本重新构建验证 |
| `tests/desktop_controls.mjs` | 原版控件绘制、开关点击、空格键、按压像素变化、拖动单次提交及下次点击通过；fieldset 禁用和无来源时禁止创建保持有效。注入上下文丢失后控件仍可点击，画布恢复后表单状态和页面标记保留，共用渲染器数量仍为 1 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 npm run test:window --prefix apps/backup-desktop` | 完整真实 Electron 回归通过：扫描、备份、还原、历史分页、失联恢复、特殊文件、单/多文件、类型筛选及 socket 路径；1120px/850px 表格和弹窗检查通过。5 次最大化/还原、最小化恢复、后台计时和关闭通过，无缺失 DRM 设备错误 |
| `env DISPLAY=:89 BACKUP_ELECTRON_PLATFORM=x11 node /tmp/backup-control-check.mjs` | Playwright 连接真实 Electron，通过点击、键盘和拖动操作。1120×760、850×600、DPR=2 截图非空；DPR=2 的开关画布宽度为 176 像素。未发现页面异常、`GL_INVALID_OPERATION` 或纹理反馈循环错误 |
| 布局复核 | 类型选择改为两列；窄窗口中新建任务内容独立滚动，标题、错误提示和确认按钮保持可见。记录表格及来源列表为玻璃按钮预留宽度，避免画布边缘造成横向溢出 |
| 来源、格式 | 着色器目录与上游 `diff -qr` 一致；许可及 NOTICE 的分发副本一致。改动的自有 TS/TSX/CSS/MJS 通过 Prettier 检查，`git diff --check` 通过 |

修复了开关动画暴露的上游 WebGL 采样器反馈循环：普通矩形和阴影着色器中未启用的 SDF 采样器绑定到占位纹理，不再误采样当前帧缓冲；同时释放该占位纹理。着色器源码未改动，适配记录已写入 NOTICE。

回归发现并修复记录/来源操作列宽不足和窄窗口操作栏遮挡。测试的点击判断改为检查类名集合，保留真实输入验证；动态记录行加载及扫描后的异步连接检查等待实际完成状态，替代对中间状态的即时断言。

截图：`/tmp/backup-controls-desktop.png`、`/tmp/backup-controls-dialog.png`、`/tmp/backup-controls-pressed.png`、`/tmp/backup-controls-narrow.png`、`/tmp/backup-controls-dpr2.png`。测试使用独立临时 Agent 配置，最终截图在隔离的 Xvfb / Openbox 显示会话运行，避免操作用户已有的窗口。DPR 由 CDP 模拟，覆盖后重新加载页面以保证画布获取新比例，未验收跨真实显示器切换或触摸屏；上下文恢复使用故障注入，不能代表真实 GPU 进程崩溃验收。白色页面按本地纯色背景渲染，观感与壁纸上的折射不同。

## 2026-10-09 原版液态玻璃与软件 WebGL

执行者：Codex 自动化运行。基于本工作区未提交版本，Ubuntu 24.04 / WSLg、Electron 44.4.5。侧栏直接集成 `martin65536/liquid-glass-webgl` 在 `0b90f9fc1cea8c7e59825f9aff1cbb96674b5b80` 的渲染器、着色器、Surface / Tinted Blue 按钮参数、背景图和弹簧动画。此实现替换了下方历史记录中的独立简化效果。主进程固定选择 SwiftShader，启动脚本不再传入 `--disable-gpu`。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件通过；TypeScript 检查和 Vite 构建通过。玻璃模块延迟加载，主包和玻璃模块分别约 278 kB，无大包警告 |
| `npm run test:window --prefix apps/backup-desktop` | 完整桌面回归通过，包括扫描、备份、还原、历史、类型筛选、多来源、850px 布局和 5 次最大化/还原。最小化恢复后 WebGL 仍可绘制 |
| `tests/desktop_glass.mjs`，由上述桌面套件调用 | 确认 SwiftShader 驱动；前两次初始化失败后自动恢复；最终屏幕帧取样非空且不同位置颜色不同；按压触发多个动画帧，弹簧收敛后停止绘制；原上下文恢复和超时替换画布均通过，页面标记及当前标签保留 |
| `node /tmp/backup-upstream-glass-check.mjs`，Playwright 连接真实 Electron CDP | 通过脚本启动 1 次、直接 `npm start` 启动 2 次，三次驱动均为 `ANGLE ... SwiftShader driver`；未出现自动回退弃用警告或缺少 DRM 设备错误。按压前后截图不同；键盘导航和减少动态效果设置下导航通过 |
| Playwright 截图、窗口与分辨率检查 | 1120×760、850×600 和 DPR=2 画面非空，玻璃按钮、图标和文字无重叠，窄窗口无文档横向溢出。DPR=2 时画布内部宽度为 CSS 宽度的两倍 |
| `diff -qr /tmp/liquid-glass-upstream/src/components/liquid-glass/shaders apps/backup-desktop/src/vendor/liquid-glass/shaders` | 所有着色器源码与上游完全一致。渲染器只包含 NOTICE 中列出的类型适配和资源释放修正 |
| 自有代码 `npx prettier --check`、`bash -n scripts/run-agent.sh`、`git diff --check` | 均通过；上游源码保留原有格式 |

截图保存在 `/tmp/backup-upstream-glass-desktop.png`、`/tmp/backup-upstream-glass-pressed.png`、`/tmp/backup-upstream-glass-switched.png`、`/tmp/backup-upstream-glass-narrow.png`、`/tmp/backup-upstream-glass-hidpi.png`。视觉检查使用独立的临时 Agent 配置；截图中的服务未连接属于该隔离环境，完整桌面套件另行启动 Server 验证真实备份和还原。

验证边界：Electron CDP 在 CSS 尺寸不变时单独覆盖 DPR，不发送 `resize` 或分辨率媒体查询变更事件；因此 DPR=2 检查在覆盖后重新加载页面，不能视为跨真实显示器切换的验收。已测试 `WEBGL_lose_context` 注入的上下文丢失，未测试真实 GPU 进程崩溃、操作系统资源耗尽、Windows/macOS 或硬件 GPU 性能。确认空闲停止绘制不代表已测得 CPU 开销或帧率指标。

## 2026-10-09 侧栏简化玻璃效果验证（历史，已替换）

执行者：Codex 自动化运行。基于本工作区未提交版本，Ubuntu 24.04 / WSLg、Electron 44.4.5。实现为本项目独立编写的 WebGL 侧栏选中态效果，未复制 AGPL-3.0 上游项目源码。WebGL 画布不接收指针事件；上下文不可用时使用可移动的 CSS 玻璃选中态。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件通过；TypeScript 检查和 Vite 构建通过 |
| `npm run test:window --prefix apps/backup-desktop` | 真实 Electron 窗口回归通过；新增检查确认玻璃画布与 CSS 回退层均不拦截输入，回退选中态尺寸有效；其余扫描、备份、还原及窄窗口流程通过 |
| Playwright 连接 Electron CDP，开启 SwiftShader 后截图与像素检查 | 1120×760、850×600 截图中侧栏高光可见，导航切换后高光从首项移到第二项；取样像素首项由 `[243,248,245]` 变为 `[235,241,237]`，第二项反向变化；850px 窗口文档宽度为 850px，无横向溢出。截图：`/tmp/backup-glass-playwright-desktop.png`、`/tmp/backup-glass-playwright-switched.png`、`/tmp/backup-glass-playwright-narrow.png` |
| `npx prettier --check`、`git diff --check` | 均通过 |

默认 WSLg 启动脚本使用 `--disable-gpu`，WebGL 不可用时使用 CSS 回退样式；单独开启 SwiftShader 可验证 WebGL 路径，但截图读取像素时 Chromium 报告 GPU stall 性能消息，因此不把该测试视为硬件 GPU 性能验收。未测 WebGL 上下文丢失时的系统级恢复、其他 Linux 显卡驱动或 Windows/macOS。

原型阶段记录：自动化测试覆盖 TCP 消息拆包、合包、非法消息，C++ Agent 与 Server 连通、端口占用、服务端关闭后的连接失败、运行目录创建，以及真实目录扫描与 SHA-256 结果。Electron 前端执行 TypeScript 类型检查和 Vite 构建。手动验证已在 WSLg/X11 打开 Electron 窗口，通过预加载接口显示服务端已连接；关闭服务端后，重新检查显示连接失败。当时目录选择 GUI、备份与还原功能尚无自动化测试结果；后两项尚未实现。以下按日期保留历史结果，最新 P0 与 FR-10 结果见 2026-10-08 的记录。

2026-09-26：修正 WSLg 无边框窗口的启动方式，默认使用原生 Wayland，并调整拖动区域与最大化状态同步。`npm run test:window --prefix apps/backup-desktop` 通过：5 次最大化/还原、11 次刷新点击、5 次目录选择入口调用，以及最小化、恢复、关闭。测试使用 Chromium 鼠标事件输入并检查真实 Electron 窗口状态；目录选择器被替换为取消结果，因此不覆盖原生文件对话框，也不覆盖 Windows 主机到 WSLg 的物理鼠标传递。

## 目标需求的测试记录

以上保留原型阶段记录。2026-10-01 的目标需求 1.0 在 [需求说明书](requirements.md) 第 6 节中设计了 AC-01 至 AC-34，包含验收、集成、单元和性能类型；其初次文档评审时均未执行。2026-10-06 已验证的基础场景见下表，其他目标继续标为未执行；文档审查和图渲染不能替代软件验收。

后续每个 AC 使用以下字段记录；没有实测证据时保持“未执行”。按 [ref/3.pdf](../ref/3.pdf) 第 21、38 页，正式报告至少涵盖两种实际测试类型及 15 个以上用例结果。

| 字段 | 执行时填写 |
| --- | --- |
| 程序版本 / 构建标识 | 待填写 |
| 模块 | 待填写 |
| 用例编号 / 名称 | 引用 AC 编号及名称 |
| 测试类型 / 级别 | 单元、集成、系统/验收或性能 |
| 预置条件 / 测试环境 | 主机、文件系统、账户权限、仓库/数据集 |
| 输入 | 实际参数与测试数据 |
| 操作步骤 | 实际执行的操作或命令 |
| 预期结果 | 引用需求中的预期 |
| 实际输出 / 结论 | 未执行 |
| 时间 / 人员 | 待填写 |
| 证据 | 日志、截图、哈希/目录比较或性能记录位置 |

## 2026-10-06 基础功能验证

执行者：Codex 自动化运行。构建标识：本工作区的基础实现，Meson 项目版本 0.1.0，尚未提交。环境：Ubuntu 24.04 / WSL2 Linux 6.6.114.1，x86_64，Intel Core i9-13900HX，GCC 13.3、Qt 6.4.2、Node.js 22.22.3；测试数据均在独立临时目录中。编译开启 Meson `warning_level=3`，本次新增 C++ 代码无编译告警。

| 命令 / 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| `./scripts/build.sh` | 6 个 Meson 测试套件通过；TypeScript 检查和 Vite 构建通过 | `build/meson-logs/testlog.txt`；各测试可由下列脚本重现 |
| `ninja -C build` 及 `meson test -C build core-test server-integration backup-integration backup-faults --print-errorlogs --logbase final-basic-regression` | 最后两处文件句柄清理与上传初始化修正后重新编译，4 个相关套件通过 | `build/meson-logs/final-basic-regression.txt` |
| `npm run test:window --prefix apps/backup-desktop` | 真实 Electron 窗口操作、备份/还原流程、重载持久化、850px 布局通过，进程正常清理 | `tests/desktop_window_smoke.mjs`；`/tmp/backup-desktop-tasks.png`、`/tmp/backup-desktop-narrow.png` |
| skill 校验与文档检查 | skill frontmatter 有效；引用与需求编号核对；`git diff --check` 无空白错误 | `.agents/skills/huawei-coding/SKILL.md`、`.clang-format`、`.prettierrc.json` |

基础测试对应关系如下。表中“通过”只覆盖列出的具体输入和故障方式，不意味着相关 AC 的所有未来扩展都已实现。

| 验证项 / 类型 | 条件、操作及实际结果 | 关联 AC / 脚本 |
| --- | --- | --- |
| 协议帧 / 单元、集成 | 拆包、合包、非法帧拒绝；真实 TCP ping/pong 请求匹配 | `protocol_test.cpp`、`server_integration.py` |
| 路径与计数 / 单元 | 拒绝绝对路径、`..`、空分量、负数、非整数、溢出及丢失精度的计数；正确区分路径前缀与包含关系 | AC-27；`core_test.cpp` |
| 无覆盖与原子配置 / 单元 | 目录 fd 拒绝软链接父路径；失败的超限 JSON 保存保持旧配置可读 | AC-01、27；`core_test.cpp` |
| 任务管理 / 集成 | 规范化重复路径只生成一项，无效目录拒绝，Agent 重启后任务仍在 | AC-01；`backup_integration.py` |
| 旧配置迁移 / 集成 | 保留原任务 ID；删除已导入任务后再次迁移不会复活任务 | AC-01；`backup_integration.py` |
| 扫描 / 集成 | 隐藏文件、中文和空格名称、空文件/目录计数及哈希正确；扫描不生成备份版本 | AC-03；`server_integration.py`、`backup_integration.py` |
| 历史版本 / 集成 | 源新增、修改、删除后生成第二版，两版分别还原并比较路径和 SHA-256，互不混入文件 | AC-04、05；`backup_integration.py` |
| 还原目标 / 集成 | 非空目录、源目录及其子目录、仓库范围、软链接根被拒绝；原文件不变 | AC-06、27；`backup_integration.py` |
| 空目录 / 集成 | 空源可以备份和还原；即使已配置源为空，也拒绝向其还原 | AC-05、06；`backup_integration.py` |
| 特殊条目边界 / 集成 | 软链接和 FIFO 警告跳过，不跟随外部链接、不阻塞读取管道 | 基础类型边界；`backup_integration.py`，不算 FR-10 已实现 |
| 源不可读 / 集成 | 文件读权限撤销导致备份失败，错误包含文件路径，旧版本可用 | AC-08；`backup_integration.py` |
| 源在读取中变化 / 故障注入 | 代理收到第一块后追加源文件，备份失败且没有新可还原版本 | AC-08；`backup_faults.py` |
| 本地认证 / 集成 | 未认证查询和错误令牌拒绝，正确令牌可访问 | 本地访问边界；`backup_integration.py` |
| 持久化及任务删除 / 集成 | Server/Agent 重启后历史可查询和还原；删除任务不删除源或版本 | AC-01、04、05；`backup_integration.py` |
| 上传中断 / 故障注入 | 丢弃连接、上传中杀死 Server，再启动只见已完成版本，旧版仍可还原 | AC-07；`backup_integration.py`、`backup_faults.py` |
| Agent 中断 / 故障注入 | 暂停传输时拒绝第二个数据请求和任务删除，但允许进度查询；杀死 Agent 后记录恢复为 INTERRUPTED | AC-07、13；`backup_faults.py` |
| 提交结果确认 / 故障注入 | 丢弃 commit 回执可查回原版本；确认也不可达时保留 WAITING；Agent 在服务端提交后退出，重启后按同一 ID 确认；重复 commit 不增加版本 | AC-34；`backup_faults.py`、`backup_integration.py` |
| 数据损坏 / 集成 | 修改存储内容字节后还原失败，当前未经验证文件不会发布 | AC-09；`backup_integration.py` |
| 非法清单 / 集成 | 清单注入 `../sentinel`，还原在创建目标前拒绝，范围外哨兵保持原样 | AC-27；`backup_integration.py` |
| 写入失败 / 故障注入 | 仓库暂存目录撤销写权限、还原目录无写权限时明确失败；旧备份仍可还原 | AC-31 的权限/写入错误部分；`backup_faults.py`、`backup_integration.py` |
| 下载中断 / 故障注入 | 中途断开还原下载，执行失败，当前临时文件被移除，不发布未校验文件 | AC-09、31；`backup_faults.py` |
| 离线行为 / 集成 | Server 停止后连接检查失败，本地扫描与配置仍可使用 | AC-30；`backup_integration.py` |
| 桌面流程 / 系统 | 原生 Electron 经鼠标事件完成任务添加、备份、版本查询和还原，输出文件内容一致；记录列表及重载持久化正确 | AC-13；`desktop_window_smoke.mjs` |
| 大文件与分页 / 性能、集成 | 64 MiB 与 1 GiB 文件往返 SHA-256 一致；120 个空目录跨多页清单均恢复；内存结果见下表 | AC-28；`backup_streaming.py` |

### 大文件结果

输入为本机临时 ext4 文件系统上的稀疏零字节数据文件，仓库实际保存完整内容；每组使用新 Agent/Server 进程，包含 120 个空目录。峰值为 `/proc/<pid>/status` 的 VmHWM，包含进程自身及扫描、备份、还原过程；不包含操作系统页缓存。

| 输入大小 | 备份耗时 | 还原耗时 | Agent 峰值 RSS | Server 峰值 RSS |
| --- | --- | --- | --- | --- |
| 64 MiB | 1.57 s | 2.03 s | 23,892 KiB | 21,940 KiB |
| 1 GiB | 21.09 s | 14.48 s | 24,920 KiB | 21,968 KiB |

文件增大 16 倍时，进程内存仍约 24/22 MiB。该结果验证本机有界分块行为，不作为跨机器吞吐量承诺。

### 验证边界

真实 ENOSPC/断电、物理鼠标到 WSLg 的传递、原生目录选择器、百万文件规模及语句覆盖率未测量。写入失败使用撤销目录权限注入，崩溃使用终止真实进程与代理丢弃响应注入；这不等同于磁盘掉电测试。P1/P2 的远端、实时、特殊文件恢复、元数据、筛选、归档、压缩、加密、回收测试仍未执行。

当前版本按完整目录树独立存储，不做对象去重；失败暂存会保留并占用空间。单个 JSON 配置或执行记录上限 1 MiB，路径集合随条目数量增长，超限应明确失败而不覆盖旧文件。以上限制在后续扩展时需继续验证。

## 2026-10-07 窗口恢复与交互回归

执行者：Codex 自动化运行，基于本工作区未提交版本。环境为 WSLg 1.0.71、Electron 44.4.5 / Wayland；有 `/dev/dxg`，没有 `/dev/dri`。Chromium 的 Wayland 初始化即使在 `--disable-gpu` 下仍枚举 DRM 节点，产生用户报告的两条 `drmGetDevices2()` 错误。启动脚本现在只在 WSLg/Wayland 且缺少 render 节点时传入空的 `--render-node-override=`，使用无 DRM 节点的软件渲染路径，不过滤错误日志。

修正前的系统层测试曾有一次恢复后页面仍报告 hidden 的超时；未证明 DRM 日志就是窗口无响应的原因。当前修正为恢复/显示/聚焦时同步窗口状态、内容焦点与重绘，并关闭 Renderer 后台节流，避免最小化或错误的遮挡状态暂停任务轮询；不会重载页面或重新启动 Agent。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 6 个核心套件、TypeScript 和 Vite 构建全部通过；含 1 GiB 备份还原校验 |
| `BACKUP_TEST_WINDOWS_INPUT=1 npm run test:window --prefix apps/backup-desktop` | 6 轮 Windows/WSLg 最小化、恢复、最大化，分别覆盖普通窗口与最大化窗口；宿主鼠标事件可点击恢复后的按钮 |
| Electron 窗口回归 | 5 轮最大化/还原、目录选择入口、最小化期间定时器运行、恢复后点击、关闭与进程清理均通过；启动输出没有缺失 DRM 设备错误 |
| 连接无响应 | 临时 TCP 服务接受连接但不回复时，页面仍能切换，添加与扫描按钮可用；扫描请求显示等待状态，连接失败后正常完成 |
| 备份操作衔接 | 真实扫描后直接备份，SHA 缩略显示且展开为完整 64 字符；完成后进入对应任务的版本列表，旧扫描详情不占据版本页面 |
| 还原确认 | 未选择路径时无法开始；选择后、确认前目标目录保持为空；确认后真实还原内容一致 |
| 移除任务 | Escape 可取消且任务保留；确认移除后源文件和历史版本保持可用 |
| 布局与持久化 | 1120×760、850×600 截图复核，无页面或版本表格横向溢出；重载后任务及记录保留，时间按中文 24 小时制显示 |
| 静态检查 | Prettier 检查、启动脚本 `bash -n` 和 `git diff --check` 通过 |

截图：`/tmp/backup-desktop-tasks.png`、`/tmp/backup-desktop-narrow.png`、`/tmp/backup-desktop-restore.png`。核心日志：`build/meson-logs/testlog.txt`。本轮 1 GiB 备份/还原分别为 23.94 / 14.72 s，Agent / Server 峰值 RSS 为 24,860 / 21,772 KiB；同轮运行图形测试，不与前次耗时作性能比较。

Windows 测试使用 Win32 `ShowWindowAsync` 和鼠标事件，经 WSLg 到 Electron；不覆盖真实物理鼠标、人工任务栏点击或所有驱动/缩放组合。原生目录选择器仍由临时路径代替。偶发窗口故障需结合这些边界判断，不能仅凭一条图形日志断言根因。

### 扫描预览与任务卡片

2026-10-07：预览按任务 ID 关联，在所属任务卡片内展示扫描进度、时间、统计、文件及警告。连续扫描保留各任务结果；备份成功或移除任务时只清理对应任务的预览。预览为当前窗口会话状态，持久化扫描记录仍可在“执行记录”查询。

`npm run test:window --prefix apps/backup-desktop` 通过：创建两个名字均为 `source`、完整路径不同的目录任务，分别扫描不同内容，其中一个包含软链接；两张卡片的路径、文件和警告正确对应。收起第一个预览时第二个仍展开，移除第二个任务不影响第一个预览；第一个预览中的备份操作产生相同任务 ID 的成功记录。还原、重载、离线及窗口既有场景继续通过。

1120×760 和 850×600 截图分别为 `/tmp/backup-scan-cards.png`、`/tmp/backup-scan-cards-narrow.png`，已检查任务卡片无横向溢出。`./scripts/build.sh`、Prettier 和 `git diff --check` 均通过。

### 扫描状态的页面范围

2026-10-07：修正扫描期间切到“备份版本”或“备份目标”仍显示顶部扫描面板的问题。扫描准备、进度及结果保留在所属任务卡片；“执行记录”仍可查看扫描详情，其他页面不显示扫描面板或准备提示。切页不清理扫描状态，后台轮询继续运行；隐藏面板的更新不触发页面滚动。

`npm run test:window --prefix apps/backup-desktop` 通过：连接检查阻塞时，扫描准备提示只出现在任务卡片及执行记录；扫描执行中切换任务、版本和目标页，每页至少继续一次进度查询，任务进度可见，版本与目标页没有扫描面板。停留在版本页收到扫描完成结果后仍不出现面板；返回任务页保留预览，执行记录可打开真实成功结果。既有备份、还原、任务移除、重载、双任务卡片及窗口操作回归继续通过。

执行中状态由测试临时包装 Electron 的 AgentBridge 查询响应来保持，真实 Agent 仍完成扫描，解除包装后返回真实结果；此方法验证页面切换和异步状态更新，不作为长扫描性能测试。`./scripts/build.sh` 的 6 个核心套件、TypeScript 和 Vite 构建通过；Prettier 与 `git diff --check` 通过。

### 执行记录按需查看

2026-10-07：进一步调整上一节的执行记录页行为。页面默认只显示记录列表，扫描准备、进度和完成均不自动展开顶部详情；点击记录的“查看”才展开，执行中的详情也可收起。当前执行状态与手动选择的记录详情分别保存：轮询更新当前记录行及已打开的同一记录，不覆盖正在查看的历史记录，也不重新打开已收起的详情。切页清除详情选择，并忽略此前未返回的查看请求；任务卡片的扫描及预览继续保留。

`./scripts/build.sh` 通过 6 个核心测试套件、TypeScript 检查及 Vite 构建。`npm run test:window --prefix apps/backup-desktop` 通过：排队和执行中的扫描均不自动打开记录详情；点击查看执行中的扫描、收起并继续轮询、重新打开后切页返回，均符合预期。新扫描期间查看上一条扫描记录，多次轮询后详情仍属于所选历史记录；关闭详情后扫描完成，仅更新列表状态。备份、还原、双任务预览、重载及窗口既有回归继续通过。执行中状态仍采用上一节的受控查询响应，不测量扫描性能。

截图 `/tmp/backup-records-during-scan.png` 已复核：执行记录页保留扫描行及状态，没有自动出现的顶部扫描面板。Prettier 和 `git diff --check` 通过。

## 2026-10-07 执行记录摘要、警告去重与分页

执行者：Codex，基于本工作区未提交实现。沿用 WSL2 / WSLg、Qt 6.4.2 与 Electron 44.4.5 环境。执行记录列表和进度查询改为摘要；警告按规范化的路径和原因排序，以 SHA-256 标识共享 JSONL 内容。旧记录在 Agent 启动时逐条整理：先持久保存并重新读取校验共享内容，再原子替换记录；整理失败保留原文件，列表及警告接口仍兼容旧格式。

记录列表及警告分页均最多 100 项，并限制每页条目内容为 64 KiB；返回 `next_offset` 表示后续位置。扫描分组在分页前覆盖全部记录，按目标、任务 ID 和源路径区分，保留各次执行时间、状态、统计和结果；备份与还原不合并。打开执行详情或警告才读取相应内容，进度轮询不携带整份警告。Agent 对超长响应返回有请求编号的小型错误，桌面接收端按 UTF-8 字节和逐行边界控制缓冲，丢弃超长响应时不终止 Agent。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 7 个 Meson 套件通过，包含新增 `operation-history`；TypeScript 检查和 Vite 构建通过，无新增 C++ 编译告警 |
| `operation-history`：旧记录及整理失败 | 构造 10 条记录，各含相同的 1,899 项警告，部分顺序相反；共享目录故意不可创建时，原文件字节保持不变，列表响应小于 16 KiB，旧警告仍能完整分页读取 |
| `operation-history`：去重与重启 | 恢复可写后 10 条记录仅引用 1 份共享明细，每次文件数仍独立；模拟共享内容已保存、记录尚未替换后重启，继续整理并复用已校验内容 |
| `operation-history`：分组及大明细 | 107 次扫描跨页无重复或遗漏，失败和中断计数正确，备份独立；420 项长路径警告合计超过 1 MiB，按字节限制分页读取全部内容。重复扫描复用警告，文件变更后的大小和哈希仍独立；新增警告产生新引用，重启后两份历史结果均可读取 |
| `operation-history`：异常边界 | 损坏共享警告、负数偏移分别返回错误，后续查询和连接检查仍可用；同步旧接口的超长结果返回受控错误，Agent PID 保持不变 |
| `npm run test:bridge --prefix apps/backup-desktop` | 独立子进程返回分段的超长 UTF-8 数据、合并正常响应、超时后的迟到超长响应；只拒绝所属请求，后台计数继续、PID 不变、后续请求正确 |
| `npm run test:window --prefix apps/backup-desktop` | 真实 Electron 通过任务、扫描、备份、版本、还原、历史详情、重载及窗口既有回归；201 项警告按 100/100/1 加载，105 次扫描默认折叠并可按 100/5 展开，异常计数在折叠状态下可见 |
| 界面与静态检查 | 1120×760 和 850×600 截图复核，表格无横向溢出；Prettier、clang-format 和 `git diff --check` 通过 |

实际旧数据另在临时副本上验证：18 条记录全部保留，记录总大小从 6,147,411 字节变为 172,933 字节，另有 1 份 284,548 字节的共享警告，合计 457,481 字节。未分组的摘要查询响应为 9,896 字节（全部 ASCII，因此字符数相同）。只复制配置和记录进行此项验证，没有启动或修改用户正在使用的 Agent 状态；实际旧数据会在更新后的客户端下次启动时整理。

证据：`build/meson-logs/testlog.txt`、`tests/operation_history.py`、`tests/agent_bridge_test.mjs`、`tests/desktop_window_smoke.mjs`；截图 `/tmp/backup-records-grouped.png`、`/tmp/backup-records-grouped-narrow.png`。未测试磁盘掉电或百万条执行历史；当前查询仍遍历记录摘要文件，共享明细不自动回收，失败整理留下的未引用明细可在后续整理时复用。本次仅调整 Agent 执行记录，Server 的版本存储格式保持原有边界。

## 2026-10-07 备份与还原状态的页面范围

原先仅限制扫描面板，备份和还原仍使用全局执行面板，准备提示也会随切页出现在无关页面。本次将备份准备、进度和结果按任务 ID 保留在任务卡片，复用执行详情的警告、待确认、查看版本及收起操作；还原面板和准备提示只出现在对应目标的版本页。执行记录仍仅在用户点击“查看”后展开，切页不丢失任务备份结果，也不因隐藏的轮询刷新页面滚动位置。

| 检查 / 命令 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 7 个核心套件、TypeScript 和 Vite 构建通过，包含 1 GiB 备份还原 |
| `npm run test:window --prefix apps/backup-desktop` | Wayland 下既有回归及新增页面范围用例全部通过；备份准备只在所属任务卡片，执行中轮询后版本/目标/记录页均无自动执行面板，记录可手动查看并收起 |
| 备份结果待确认 | 真实版本提交后，将临时测试记录设为 WAITING，切到目标页后解除轮询保持；返回任务仍显示待确认，点击卡片按钮通过真实 Agent/Server 查询确认成功，清理对应旧扫描预览 |
| 还原与切页 | 还原准备和执行中遍历任务/目标/记录页，均不出现还原面板；在目标页完成后返回版本页，真实成功结果仍在，还原内容与源一致 |
| 结果保留与收起 | 还原期间保留任务的备份结果，返回任务可收起，切页返回不重新打开；执行记录及分组、双任务扫描、重载、窗口控制既有用例通过 |
| 布局及静态检查 | 1120×760、850×600 备份卡片截图复核，页面及卡片无横向溢出；Prettier 和 `git diff --check` 通过 |

准备状态用测试内的请求门控保持，执行中用 AgentBridge 响应包装保持；真实 Agent 仍执行备份和还原，解除包装后读取真实结果。WAITING 注入仅修改临时测试记录，不模拟真实网络丢包；底层提交回执丢失由既有 `backup-faults` 覆盖。截图为 `/tmp/backup-desktop-tasks.png` 和 `/tmp/backup-task-result-narrow.png`。未修改用户正在使用的配置、任务或仓库。

### 同轮 WSLg 窗口不可见排查

用户报告启动脚本完成编译后没有窗口。检查确认 Electron 主进程、Renderer、C++ Agent 均在运行，Windows 存在 `[WARN:COPY MODE] Backup System (Ubuntu-24.04)` 窗口。Win32 报告可见、未最小化且可设为前台，但用户仍看不到；窗口区域的宿主截图也未显示应用内容。`/mnt/wslg/weston.log` 在本次图形会话初始化时记录 `rdp_allocate_shared_memory` 打开共享内存失败，错误为 `Input/output error`，随后 `use_gfxredir = 0`。

默认 Wayland 下的 Chromium 输入、页面截图和工作流回归通过；这不能证明 Windows 实际显示链路正常。对照命令 `BACKUP_ELECTRON_PLATFORM=x11 npm run test:window --prefix apps/backup-desktop` 完成工作流检查后，在最小化步骤超时，因此未切换默认显示模式。当前证据提示 WSLg/宿主显示异常，尚未通过重启验证共享内存错误是否为唯一根因。本轮未停止用户的 Agent/Server、未重启 WSL，README 已补充保存工作和停止任务后重启图形环境的排障步骤；实际窗口恢复仍待验证。

## 2026-10-08 P0 缺口补齐

范围：用户确认本轮只补 P0，导出留到 P2。沿用 Qt 6.4.2、Electron 44.4.5、WSL2 / WSLg 环境，基于工作区未提交改动。遵循项目 huawei-coding skill，测试均使用独立临时配置、源目录及仓库，没有重启用户的客户端、Server 或 WSL。

| 检查 / 命令 | 实际结果及对应范围 |
| --- | --- |
| `./scripts/build.sh` | 9 个 Meson 套件全部通过，0 跳过；TypeScript 和 Vite 构建通过，无新增 C++ 编译告警。包含原有协议、路径安全、迁移、备份还原、故障、历史记录和大文件回归 |
| `backup-p0-boundaries`：FR-01 / FR-02 | 真实上传暂停时保存目标新端口，当前操作使用原快照并成功，下次操作使用新端口；仓库路径不匹配、源/仓库交叉、别名目标重复源、修改目标导致重复任务均拒绝。存储不可写时 health 返回 unavailable，恢复权限后可用；不可列举的版本目录返回查询错误，不伪装成空仓库 |
| `backup-p0-boundaries`：FR-03 | 两个无读取权限文件、一个无权限目录均汇总到同一不完整扫描，其他可读文件继续计算 SHA-256；文件、目录、软链接带类型；失效根目录保留失败预览；扫描不生成版本 |
| `backup-p0-boundaries`：FR-04 / FR-05 | 同一目录真实备份 10 次，每次 1,899 项长路径警告，单版明细超过 1 MiB；列表只含摘要、warning_count 与保存的 rules，每页最多 100 项并按字节限制。旧内嵌警告版本仍可分页、还原；重启、源内容变化后的新旧还原分别正确。损坏警告只使明细查询失败，列表仍可用 |
| `backup-p0-boundaries`：FR-06 / FR-17 | 125 个文件的版本在第 111 个文件损坏时失败，写入明细分页列出 110 项已写入及 1 项待核对，与磁盘内容匹配；正常还原列出 125 项。下载中杀死 Agent，再模拟日志尾部撕裂，重启恢复 INTERRUPTED，保留 2 项已写入、1 项待核对和残留临时文件路径；还原也拒绝其他已配置仓库 |
| `backup-disk-full`：FR-04 / FR-06 | 独立 user/mount namespace 内创建 8 MiB 仓库 tmpfs、2 MiB 还原 tmpfs，真实写入触发 ENOSPC。新备份失败且不发布，存储检查显示不可用，旧版仍可还原；还原失败带写入明细，未经验证文件与临时文件未留下，原版本再次还原成功。此次未跳过，不是仅模拟权限错误 |
| `npm run test:bridge --prefix apps/backup-desktop` | 超大 UTF-8 响应只拒绝所属请求；后台计数、PID、后续帧与查询继续正常 |
| `npm run test:window --prefix apps/backup-desktop` | 真实 Electron 全流程通过，新增用例由 `desktop_p0_checks.mjs` 执行：第一次状态查询即失败，三次后停止自动重试、显示失联并保留操作锁；重载页面后仍按持久化摘要恢复所属任务的失联显示，手动重查取回真实结果。执行期间仍可编辑目标并提示下次生效；不完整预览汇总两项错误、类型列可见，预览备份按钮禁用；存储不可用仍可查询历史版本；201 项旧版本警告按 100/100/1 加载；还原写入明细可见 |
| 最后配置校验后的定向回归 | `source scripts/qt-env.sh` 后运行 `meson test -C build backup-p0-boundaries backup-integration backup-faults --print-errorlogs --logbase p0-final`，3 套件全部通过 |
| 布局与格式 | 1120×760、850×600 截图复核，扫描表格、目标表单及失联提示无横向溢出；改动 C++ 的 clang-format 检查、TS/TSX/CSS/MJS 的 Prettier 检查与 `git diff --check` 通过 |

本轮 1 GiB 备份 / 还原为 22.82 / 16.43 秒，Agent / Server 峰值 RSS 为 25,184 / 22,460 KiB，仅代表本机样本。证据：`build/meson-logs/testlog.txt`、`build/meson-logs/p0-final.txt`；新增测试为 `tests/backup_p0_boundaries.py`、`tests/backup_disk_full.py`、`tests/desktop_p0_checks.mjs`。截图在 `/tmp/backup-operation-lost.png`、`/tmp/backup-targets-p0.png`、`/tmp/backup-incomplete-1120.png` 和 `/tmp/backup-incomplete-850.png`。

验证边界：物理掉电、存储控制器缓存失效和百万条目规模未验证，进程 kill 与 tmpfs 写满不能代替掉电验收。旧还原记录未保存过逐项写入数据，不能追补；新还原才有持久化写入日志。分页控制响应大小，但记录/路径集合仍随条目数增长；当前不自动清理历史、共享警告或失败暂存。新警告接口需要客户端和 Server 同时更新，已有仓库继续读取，不能将新版本直接交给旧程序解释。

本轮首次 Wayland 窗口回归在最大化/还原图标同步处超时，后续完整回归通过，含最小化恢复。保留上一节关于 Windows 实际窗口不可见的未解决边界：页面截图和自动化通过不等同于用户宿主窗口已恢复；未重启 WSL，也未宣称该显示问题已修复。

## 2026-10-08 FR-10 文件类型支持

执行者：Codex 自动化运行。构建标识：基于 `cad08c6` 的本工作区未提交改动，项目版本 0.1.0。遵循 huawei-coding skill；环境为 GCC 13.3、Qt 6.4.2、Node.js 22、Electron 44.4.5、WSL2 / WSLg。所有源目录、仓库、挂载点与 Agent 状态均为独立测试数据。

| 命令 / 验证项 | 实际结果及对应范围 |
| --- | --- |
| `./scripts/build.sh` | 11 个 Meson 套件全部通过，0 失败、0 跳过；C++ 无新增编译告警，TypeScript 检查与 Vite 构建通过。包含原有 P0、真实磁盘写满、故障、历史分页与大文件回归 |
| `backup-file-types`：软链接 / AC-17 | 相对、绝对、外部、目录、悬空、自引用、循环引用、3,000 字节长目标及非 UTF-8 目标共 10 个软链接；扫描不遍历目标，备份清单 Base64 解码与原字节完全一致，源修改及服务重启后 readlink 仍匹配原值 |
| `backup-file-types`：硬链接 / AC-17 | 三成员组、跨目录组、空文件组、范围外成员及只有单成员入选的组均验证；8 个普通文件路径只保存 5 份内容，内容相同但 inode 不同的文件仍独立存储。还原同组成员共享 inode，且不与源目录或范围外成员关联；files/bytes 按路径计数，stored_bytes 与仓库内容文件字节之和一致 |
| `backup-file-types`：FIFO、持久化 / AC-17、FR-17 | 两个 FIFO（含隐藏名称）扫描、备份、还原均完成且类型保持 FIFO，没有读取流导致阻塞。删除任务、重启 Agent/Server 后版本仍可还原；22 个还原条目的类型和 written 状态分页可查，Agent 重启后写入明细保持一致 |
| `backup-file-types`：兼容及非法清单 / AC-27 | 格式 1 的普通文件版本及旧内嵌警告仍可查询/还原；格式 2 支持新类型。25 份恶意格式 2 清单及 1 份夹带特殊类型的格式 1 清单在创建目标前拒绝，覆盖链接父目录、绝对/越界路径、NUL/错误编码/超长目标、组外/前向/循环/链式硬链接、错误大小/哈希、重复路径及不支持节点。Server 也拒绝非法新条目 |
| `backup-file-types`：竞态及失败 / AC-08、27、FR-10 | 上传已记录硬链接后修改组内源内容，新版本不发布。还原时替换或改写已校验的组首文件，后续链接失败；将父目录替换为外部软链接时，普通文件、软链接、FIFO、硬链接均不能写出目标，外部哨兵不变。测试专用 LD_PRELOAD 库对三类创建调用注入 EOPNOTSUPP，均明确失败、保留 pending/written 明细，不退化成普通文件或复制；撤除注入后原版本可正常还原 |
| `backup-file-type-mounts`：不支持节点 / AC-18 | 私有 user/mount namespace 中以只读、nodev 绑定暴露真实字符设备和块设备元数据，并创建 socket；扫描识别三种类型，备份标记完成并有警告且明细含类型和原因，还原不创建这些节点，不打开设备流 |
| `backup-file-type-mounts`：跨设备 / FR-10 | 在还原到组内另一路径前，将其父目录挂载到独立 tmpfs，实际 linkat 返回 EXDEV；失败包含相对路径和系统原因，已恢复的组首内容保留，别名不被复制为普通文件，日志保留待核对项 |
| `npm run test:window --prefix apps/backup-desktop` | 完整真实 Electron 回归通过。新增 `desktop_file_types.mjs` 通过鼠标事件执行扫描、备份及还原，验证类型列、链接目标/同组路径、特殊计数、逻辑大小 38 B 与内容存储 19 B、还原确认及类型明细，并在磁盘核对 inode、readlink、FIFO。1120×760 和 850×600 截图复核，长目标文本换行、表格无横向溢出；最小化/恢复及关闭继续通过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 超大 UTF-8 响应只影响该次查询，Agent、后台工作及后续请求继续正常 |
| 格式与差异 | 改动 C++ 的 `clang-format --dry-run --Werror`、TS/TSX/MJS 的 `prettier --check` 及 `git diff --check` 通过 |

核心证据为 `build/meson-logs/testlog.txt`，测试代码为 `tests/backup_file_types.py`、`tests/backup_file_type_mounts.py`、`tests/restore_syscall_faults.cpp`、`tests/desktop_file_types.mjs`。桌面截图在 `/tmp/backup-types-preview-rows-1120.png`、`/tmp/backup-types-preview-rows-850.png`、`/tmp/backup-types-version-850.png` 和 `/tmp/backup-types-restore-850.png`。大文件回归中 1 GiB 备份/还原为 23.47/16.93 秒，Agent/Server 峰值 RSS 为 24,684/22,440 KiB，仅代表本机样本。

测试调整记录：原有大量警告用例改用不支持的 socket，保留原有数量和分页压力；软链接与 FIFO 不再作为跳过项。私有命名空间禁止 mknod，因此设备识别采用只读、nodev 绑定，未读取真实设备内容。一次新增桌面测试在目录选择后立即点击确认而超时；补上选择结果返回与按钮启用的等待后完整回归通过。

边界：跨设备失败已实测；EOPNOTSUPP 是系统调用故障注入，不代表对全部文件系统逐一验收。依赖 Linux procfs、目录 fd 与 POSIX 节点操作；本轮未实现 FR-12 权限、属主和时间恢复，也不提供源目录原子快照。新备份写格式 2，旧格式 1 保持可读且不改写，旧版未保存的链接关系不能补回；客户端与 Server 需同时更新。物理掉电、百万条目及宿主实际窗口显示边界仍沿用前述记录。

## 2026-10-08 单文件、多文件与文件夹来源

执行者：Codex 自动化运行。构建标识：基于 `cad08c6` 的工作区改动，保留前述 FR-10 实现，尚未提交。沿用本页的 Linux / WSLg 环境及 huawei-coding 规范；所有测试使用独立临时配置、源文件和仓库。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 12 个 Meson 套件全部通过，0 失败、0 跳过；C++ 编译、TypeScript 检查和 Vite 构建通过。包含真实 ENOSPC、传输中断、旧版本、FR-10 和 1 GiB 流式备份/还原 |
| `backup-sources` / AC-35 | 12 组真实进程场景通过：旧整目录保持原还原结构；单文件不读取不可读同级项；跨目录同名文件保留层级；多文件夹及混合选择保留隐藏文件/空目录；重复及被包含的来源合并；不同选择范围可分别建任务 |
| 类型、权限和变化 | 显式软链接、FIFO、硬链接组维持 FR-10 语义；所选条目缺失、文件变成目录、任务根被替换为软链接时失败；未选同级项变化不影响备份，所选内容传输中变化不发布版本。父目录仅有搜索权限（0111）时，选定的可读文件仍能备份和还原，不要求列举目录 |
| 范围隔离和持久化 | 检查 Agent/仓库重叠及空/超量/缺失输入；Server 拒绝范围外条目和漏传选定项的提交；篡改版本选择范围时在创建还原目录前拒绝。配置从 1 升级为 2 后保留旧任务标识，非法旧配置导入保留原状态；Agent/Server 重启、源修改和任务删除后，历史版本仍按原选择还原 |
| 最后定向回归 | `source scripts/qt-env.sh` 后运行 `meson test -C build backup-sources operation-history --print-errorlogs --logbase sources-final`，2 套件全部通过；覆盖最后的系统错误捕获、配置导入检查及执行摘要范围字段 |
| `npm run test:window --prefix apps/backup-desktop` | 完整 Electron 回归通过。`desktop_sources.mjs` 覆盖文件/文件夹选择入口、多选参数、取消后保留列表、可选任务名称、超长文件名、混合来源合并、移除、错误恢复和页面重载；单文件及两个同名文件通过真实扫描、备份、版本筛选、确认还原，磁盘内容和未选项隔离均匹配 |
| 布局与兼容交互 | 1120×760、850×600 截图复核新建任务、版本来源与还原确认。长名称和路径换行、来源列表限高滚动，按钮可见且无横向溢出。原有状态归属、警告分页、失联反馈、FR-10、最小化/恢复/关闭回归继续通过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过；超长响应只失败当前查询，后续查询与后台工作继续运行 |
| 格式 | 改动 C++ 的 `clang-format --dry-run --Werror`、TS/TSX/CSS/MJS 的 `prettier --check` 和 `git diff --check` 通过 |

证据：`build/meson-logs/testlog.txt`、`build/meson-logs/sources-final.txt`；测试代码为 `tests/backup_sources.py`、`tests/desktop_sources.mjs`。截图为 `/tmp/backup-sources-{single-composer,multiple-composer,mixed-composer,task,version,restore-dialog}-{1120,850}.png`。本轮大文件样本的 1 GiB 备份/还原为 20.62/15.33 秒，Agent/Server 峰值 RSS 为 24,456/22,360 KiB，仅代表本机本次运行。

测试发现并修复：选定文件最初仍要求父目录的列举权限，新增 0111 父目录用例先失败，改为以 O_PATH 访问结构父目录后通过，同时保留逐级 O_NOFOLLOW 和实际文件夹遍历所需的读取权限。新增测试初次还修正了协议中字符串计数的断言，以及仓库已知路径的测试前置条件。

边界：每任务最多 100 个显式来源、选择记录最多 16 KiB；文件夹内后代不占此项数。多项来源按共同父目录映射，可能保留多层目录，创建前有路径预览。旧目录配置可读，首次保存选择任务时原子升级到配置格式 2，旧 Agent 会拒绝；客户端与 Server 需同时更新。原生文件选择器由自动化提供路径，仅检查调用参数及取消语义，未人工验收系统选择器。未实现 FR-11 的条件筛选或文件系统原子快照；物理掉电、百万条目及宿主窗口实际显示的验证边界仍保留。

## 2026-10-08 新建任务文件类型筛选

执行者：Codex 自动化运行。基于本工作区未提交的单文件/多文件及 FR-10 改动，遵循 huawei-coding skill；测试使用独立临时源、仓库和 Agent 配置。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件全部通过，0 失败、0 跳过；C++ 编译、TypeScript 检查及 Vite 构建通过，含 1 GiB 流式回归 |
| `backup-type-filter` / AC-36 | 真实 Agent/Server 场景通过：普通文件、目录、软链接、FIFO 分别筛选；排除目录类型时保留必要父目录，筛选为空仍可发布和还原；直接选中的多个文件逐项校验，非法/重复规则不写配置；Server 拒绝被排除的条目和不匹配的直接选择；篡改历史规则在还原前拒绝；Agent/Server 重启和任务删除后规则仍可查询与还原 |
| `npm run test:window --prefix apps/backup-desktop` | 完整 Electron 回归通过。新增 `desktop_type_filter.mjs` 用真实鼠标事件勾选类型，完成文件夹扫描、备份、版本查询和还原；多项直接文件有一项不匹配时禁止创建，移除该项及再次切换规则后可恢复。原有 FR-10、单/多文件、窗口恢复及记录交互继续通过 |
| 窗口布局 | 1120×760、850×600 截图复核新建任务及错误状态；窄窗来源列表独立滚动，错误和操作按钮不重叠，按钮保持可见，无横向溢出 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过；超大响应只失败对应查询，后续请求和后台工作继续 |
| 格式与差异 | 本轮 TSX/CSS/MJS 的 Prettier 检查及 `git diff --check` 通过；本机未安装 `clang-format`，未执行其独立检查，新增 C++ 代码已编译且检查了格式 |

证据：`build/meson-logs/testlog.txt`、`tests/backup_type_filter.py`、`tests/desktop_type_filter.mjs`；截图为 `/tmp/backup-type-filter-{folder-composer,direct-mismatch}-{1120,850}.png`。文件类型筛选仅覆盖四种可备份 Linux 条目类型，不代表 FR-11 的路径模式、大小、时间等条件筛选已实现；原生文件选择器仍由测试注入返回路径，未人工操作系统对话框。

## 2026-10-08 目录遍历与空目录规则分离

执行者：Codex 自动化运行。基于本工作区未提交改动；测试在隔离的临时源、仓库与 Agent 状态目录进行。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件全部通过，0 失败、0 跳过；C++、TypeScript 和 Vite 构建通过，包含 1 GiB 流式备份/还原回归 |
| `backup-type-filter` / AC-36 | 新任务仅接受普通文件、软链接、FIFO 类型且至少一项；目录始终递归遍历。默认保留空目录，关闭后仅保留匹配条目的父目录；两种规则生成的版本分别还原正确。无匹配文件的文件夹任务仍可创建、扫描、备份、还原；嵌套 socket 仍报告跳过警告，不将仅有警告的空目录写进精简版本 |
| 兼容与校验 | 旧“仅目录”版本保留原语义；格式 4 配置中再添加旧格式任务不会降级。Server 拒绝关闭空目录保留规则下多余的空目录，Agent 在还原前复核清单和版本规则；直接选择类型不匹配的文件时创建失败 |
| `npm run test:window --prefix apps/backup-desktop` | 完整 Electron 回归通过：新建界面将三种内容类型与独立的“保留空目录”分开，默认开启；切换规则后重新校验，扫描无匹配文件显示提示但仍允许备份；普通文件筛选的真实版本还原正确。原有任务/记录/窗口恢复交互继续通过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过，超大查询响应不影响后续请求和后台工作 |
| 格式 | 本轮涉及的 TS/TSX/CSS/MJS 通过 Prettier 检查，`git diff --check` 通过；本机未安装 `clang-format`，未执行独立 C++ 格式检查 |

截图：`/tmp/backup-type-filter-{folder-composer,direct-mismatch}-{1120,850}.png`。自动化使用注入的文件选择结果，未人工验收操作系统原生选择器；仅实现 Linux 条目类型筛选与空目录开关，尚未实现 FR-11 的文件名、扩展名、时间、大小等条件规则。物理掉电和百万条目规模仍未验证。

## 2026-10-09 设备与 socket 节点保存

执行者：Codex 自动化运行。测试使用隔离的临时源、仓库、Agent 配置及私有挂载命名空间；默认任务仍跳过设备和 socket，显式选择后只保存节点信息。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | 13 个 Meson 套件通过，0 失败、0 跳过；C++ 无新增编译告警，TypeScript 和 Vite 构建通过 |
| `backup-file-type-mounts` | 真实字符/块设备的类型及主/次设备号和 Unix socket 节点进入格式 3 清单，未生成内容对象；默认任务仍报告 3 项跳过警告。本机可创建字符设备，不能创建样本块设备，故还原在块设备处报告 `Operation not permitted` 并留下 pending/written 日志；有权创建块设备环境下的完整设备还原分支未在本机实测 |
| `backup-file-types` / `backup-type-filter` | 格式 1、2 历史版本仍可还原；缺失、越界及类型不匹配的设备字段和旧格式特殊节点在写入前拒绝。选中 socket 的目录任务与直接路径任务可扫描、备份并重建未连接节点；注入 `EOPNOTSUPP` 和替换父目录的测试明确失败且不写出目标目录 |
| `npm run test:window --prefix apps/backup-desktop` | 完整窗口回归通过。新增类型选择区、直接输入 socket 绝对路径、扫描预览和任务持久化通过；1120×760、850×600 截图确认常用流程的来源列表、移除按钮和操作区可用 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过，超限查询不终止后台工作 |
| 格式 | 涉及前端文件的 Prettier 检查和 `git diff --check` 通过；本机未安装 `clang-format`，未执行独立 C++ 格式检查 |

证据：`build/meson-logs/testlog.txt`、`tests/backup_file_type_mounts.py`、`tests/backup_file_types.py`、`tests/backup_type_filter.py`、`tests/desktop_type_filter.mjs`；截图为 `/tmp/backup-type-filter-socket-composer-{1120,850}.png`。设备节点只保存编号，不是整盘镜像；socket 不保存连接、监听状态或进程。还原不提权，目标文件系统不支持节点创建时会失败并保留已写入范围。物理掉电及不同 Linux 文件系统上的设备创建仍未验收。
