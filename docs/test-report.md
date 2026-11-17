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

## 2026-10-19 移除套接字节点备份

执行者：Codex。范围仅限文件系统 Unix socket 节点；TCP 通信与历史版本还原保留。新建任务不提供 socket 类型，直接路径选择在 Agent 拒绝；目录扫描报告跳过，旧含 socket 类型的任务配置保持可读但扫描/备份明确失败。Server 的 begin 与 entry 两处拒绝新 socket 上传。测试已改为覆盖拒绝和旧版本兼容，但当前环境未能运行其集成断言。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | C++ 重新编译、链接成功；Meson 13 套件中协议和核心 2 个通过，挂载测试 1 个跳过，其他 10 个均在 Server 启动时失败，错误为 `LISTEN_ERROR Unknown error`，未进入业务断言；脚本因此未运行到前端构建 |
| `ninja -C build`（设置 Qt 库路径） | 最后一次 C++ 修改重新编译并链接成功。直接不设置 Qt 库路径时曾因找不到 `libpxbackend-1.0.so` 链接失败，设置项目 Qt 路径后恢复 |
| `meson test -C build protocol-test core-test --print-errorlogs`（设置 Qt 库路径） | 2/2 通过 |
| `npm run build --prefix apps/backup-desktop` | TypeScript 检查和 Vite 构建通过 |
| `node --check tests/desktop_type_filter.mjs`、`python3 -m py_compile`（三份修改的 Python 测试）、`git diff --check` | 均通过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 两次运行均失败：测试子进程提前退出，桥接层报告 `Backup Agent 已退出（代码 0）`；该失败未定位为本次改动，不能据此声称桥接回归通过 |
| 网络集成与桌面回归 | 未完成。两次申请在允许本机回环监听的环境中重跑，审批服务均返回 503，命令未执行；当前沙箱中 Server 无法监听测试端口 |

待在允许本机回环监听的环境中重跑 `./scripts/build.sh` 和 `npm run test:window --prefix apps/backup-desktop`，核对新任务拒绝、Server 拒绝、旧配置保留及历史 socket 版本还原。`backup-file-type-mounts` 还需要支持私有挂载命名空间及可用块设备节点的 Linux 环境。

## 2026-10-19 新建任务弹窗改为纯色表面

执行者：Codex。只调整新建任务弹窗：不挂载弹窗、文本框、下拉框及文件类型开关的玻璃画布；关闭、添加、取消和创建等按钮继续使用液态玻璃组件。其他工作流弹窗保持原有样式。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `npm run build --prefix apps/backup-desktop` | TypeScript 检查及 Vite 构建通过 |
| `node --check tests/desktop_type_filter.mjs`、`git diff --check` | 均通过 |
| `npm run test:window --prefix apps/backup-desktop` | 未运行到界面断言：测试准备阶段创建 Unix socket 节点时收到 `PermissionError: Operation not permitted` |
| 带本机 socket 权限的窗口回归 | 已申请执行，但自动审批服务返回 503，命令未执行；没有取得本轮窗口截图或视觉验收结果 |

桌面回归新增断言会检查新建任务弹窗无玻璃面板/字段/开关画布、按钮仍有玻璃画布；需在允许创建 Unix socket 节点的环境中补跑。

## 2026-10-19 新建任务开关恢复液态玻璃

执行者：Codex。根据反馈，恢复普通文件、软链接、FIFO、常用全选和保留空目录的 `GlassSwitch`，展开后的设备类型也使用同一组件。新建任务弹窗仍为纯色背景，输入框和下拉框仍为普通控件；操作按钮继续使用 `GlassButton`。本节覆盖上一节中“文件类型开关不挂载玻璃画布”的描述。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `npm run build --prefix apps/backup-desktop` | TypeScript 检查及 Vite 构建通过 |
| `npm run test:window --prefix apps/backup-desktop` | 测试准备阶段创建 Unix socket 节点时收到 `PermissionError: Operation not permitted`，未运行到新增的弹窗断言 |
| 带本机 socket 权限的窗口回归 | 自动审批服务返回 503，命令未执行；本轮未取得截图或视觉验收结果 |

桌面回归断言已改为要求纯色弹窗内保留液态玻璃按钮和开关画布。需在允许创建 Unix socket 节点的环境中补跑。

## 2026-10-19 工作流弹窗背景改为普通面板

执行者：Codex。移除任务和还原版本弹窗不再挂载整面液态玻璃画布，与此前的新建任务弹窗统一使用白色面板；按钮、开关、输入框和侧栏的玻璃效果保留。删除仅供弹窗背景使用的渲染分支，并为移除任务确认弹窗添加外观断言。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `npm run build --prefix apps/backup-desktop` | TypeScript 检查及 Vite 构建通过 |
| `node --check tests/desktop_window_smoke.mjs`、`git diff --check` | 均通过 |
| `node tests/desktop_window_smoke.mjs` | 测试准备阶段创建 Unix socket 节点时收到 `PermissionError: Operation not permitted`，未运行到弹窗断言 |
| 带本机 socket 权限的窗口回归 | 自动审批服务调用 `gpt-5.6-luna` 返回 503，命令未执行；本轮未取得窗口截图 |

需在允许创建 Unix socket 节点的环境中补跑完整桌面回归，确认删除及还原弹窗的实际视觉效果。

## 2026-10-25 单个备份版本删除

执行者：Codex 自动化运行。基于 `bfa17ac` 的未提交工作区，保留已有的 socket 备份移除和普通弹窗背景改动，按 huawei-coding 规范实现。所有备份、删除及故障注入均位于独立临时目录，没有删除用户仓库。范围为 FR-16 手动单版删除和 FR-17 执行反馈，对应 AC-38；不包含保留策略或自动回收。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | C++ 编译及链接通过，无新增编译警告；14 个 Meson 套件全部通过，0 失败、0 跳过；TypeScript 检查与 Vite 构建通过。包含真实 ENOSPC、旧版本兼容、FR-10、类型筛选和 1 GiB 流式备份还原 |
| `version-deletion` / AC-38 | 通过，完整构建中耗时 28.07 秒。实际回收所选版本目录，源文件及其他版本保持不变，其他版本可还原；删除任务后仍可删版本，重复请求成功且不能用已删 ID 再次上传 |
| 删除边界 | 未认证、非法 ID、错误仓库、链接版本目录均拒绝；真实读取连接持有版本时删除失败，断开后可删除；内容软链接只移除链接本身，外部哨兵文件不变 |
| 中断与重试 | 超过 300 个文件触发多批回收；首批后重启 Server 可继续。删除回执被丢弃时记录 WAITING，修改当前目标后重试仍使用原目标快照；杀死 Agent 后重启恢复 WAITING，再次确认可完成。回执写入失败时版本保留，移出列表后的权限失败可修复并继续；结果记录跨 Agent 重启保留 |
| `node tests/desktop_window_smoke.mjs` | 最终完整回归通过。新建两个真实版本，核对确认摘要、不可撤销提示和“取消”默认焦点；取消不删除；发起失败时弹窗保留错误；实际撤销删除目录写权限造成执行失败，从执行记录打开“重试删除”并确认后成功；版本列表刷新、源与任务保留、记录重载及剩余版本实际还原均通过 |
| 桌面布局 | 查看 1120×760、850×600 截图，确认弹窗为白色普通面板、按钮有玻璃画布，完整版本 ID 与路径可辨，无横向溢出，底部按钮可见。既有玻璃像素、上下文恢复、拖动/键盘、最小化/恢复和页面状态归属检查继续通过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过：超大 UTF-8 回复只失败对应查询，后续请求及后台工作继续 |
| 格式与差异 | 新 C++ 文件及已修改代码段通过 `clang-format --dry-run --Werror`；本轮前端和桌面测试通过 `prettier --check`；`git diff --check` 通过 |

证据：`build/meson-logs/testlog.txt`、`tests/version_deletion.py`、`tests/desktop_version_deletion.mjs`；确认窗口截图为 `/tmp/backup-delete-confirm-{1120,850}.png`。本轮也完成了 2026-10-19 因环境权限未能运行的 socket 移除、普通面板与玻璃按钮/开关回归；对旧历史结果不作改写。

测试过程记录：初次权限测试错误地在原版本目录撤销写权限，导致改名先失败，已改为首批移出后再撤销权限，以验证中途回收失败。GUI 发现 React 的 autoFocus 在原生 dialog 打开时被关闭图标抢占，改为打开后明确聚焦“取消”，回归通过。旧玻璃测试的整面玻璃和 8 个开关断言，与此前已授权的普通背景、移除 socket 选项不符，已更新。最终补测前有一次既有 WebGL 创建故障注入等待超时；未修改玻璃实现，重新运行完整套件通过。

边界：删除完成会保留小型持久回执和执行历史；已有 Agent 共享警告与失败备份暂存不在删除范围。中断后部分内容可能已回收，用户需从记录重试，不提供撤销或自动启动清理。实际空间回收依赖文件系统，界面显示版本逻辑大小，不承诺等量的物理释放量。磁盘完全写满、无法保存删除意图时会失败，不绕过持久化保证；本轮未专门对删除注入 ENOSPC，也未验证物理断电或百万条目规模。需要同时重启客户端和 Server 加载新接口。

## 2026-10-25 课程 Huffman / LZ77 压缩备份

执行者：Codex 自动化运行，基于 `8d7a602` 的未提交工作区。按用户提供的打包、压缩 PPT 和实验指导书自行实现两种算法，替代需求中的原定 zstd；对应 FR-14、AC-24、AC-32 及 AC-28 的资源约束。课件对应页码和格式见 [compression.md](compression.md)。全部业务测试使用独立临时仓库，未改写用户历史版本。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | C++ 编译链接完成，无新增编译警告；17 个 Meson 套件全部通过，0 失败、0 跳过；TypeScript 检查与 Vite 构建通过。压缩大文件套件耗时 275.66 秒 |
| `compression-test` | 通过，0.31 秒。覆盖二进制数据、频率并列、单符号、1/7/8/9 字节等边界、位补零、LZ77 重叠复制、不可压缩数据回退和确定性；拒绝错误算法、越界长度、非法词频/位数/距离、截断与尾字节 |
| `backup-compression` | 通过，15.82 秒。含 4 MiB 可压缩内容、随机与全字节数据、中文、空文件、空目录、软/硬链接和 FIFO；原未压缩内容为 4,993,999 字节，Huffman 为 1,335,324 字节，LZ77 为 596,513 字节。各组真实还原后 SHA-256、类型与硬链接 inode 关系正确 |
| 配置和异常 | 启用后保存配置格式 7，新增设备类型任务不降级，Agent 重启保留策略；目标改回 none 后仍可还原压缩版及旧格式。篡改块摘要、长度、编码、截断或追加数据时失败，不发布受损文件；上传中修改正在读取的文件、断开传输均不发布版本；丢弃 commit 回执后按原 ID 确认，无重复版本。无效压缩配置原子拒绝，Server 拒绝未知算法 |
| `npm run test:window --prefix apps/backup-desktop` | 最终完整回归通过；在真实 Electron 中选择两种压缩策略，保存、重载后策略正确，实际创建压缩版本并自动解压还原，内容一致；版本显示算法，1120×760 和 850×600 无横向溢出。原有玻璃交互、来源选择、类型筛选、删除、最小化/恢复与退出检查也通过 |
| 桌面外观 | 查看 Huffman 与 LZ77 的窗口截图，压缩选项、提示、端口及保存按钮可辨；小窗口可通过正常纵向滚动访问完整表单。截图为 `/tmp/backup-compression-{huffman,lz77}-{1120,850}.png` |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过，超大 UTF-8 回复只失败对应查询，后续请求和后台操作继续 |
| 格式检查 | 新 C++ 文件及变更代码段通过 `clang-format 18.1.8 --dry-run --Werror`；本轮修改的前端与桌面测试通过 Prettier 检查；`git diff --check` 通过 |

`compression-streaming` 的实测结果如下。样本为全零稀疏源文件，时间为备份及还原合计；存储字节仅统计 `.data` 内容。每次都比较还原文件 SHA-256，不以能打开文件代替数据验证。这些结果不能外推到随机数据或已压缩媒体。

| 算法 | 原文 MiB | 存储字节 | 耗时秒 | Agent 峰值 KiB | Server 峰值 KiB |
| --- | --- | --- | --- | --- | --- |
| Huffman | 64 | 8,663,296 | 12.33 | 24,672 | 22,700 |
| Huffman | 1024 | 138,612,736 | 156.74 | 24,664 | 22,256 |
| LZ77 | 64 | 1,061,120 | 4.79 | 24,636 | 23,228 |
| LZ77 | 1024 | 16,977,920 | 72.05 | 25,376 | 23,228 |

证据：`build/meson-logs/testlog.txt`、`tests/compression_test.cpp`、`tests/backup_compression.py`、`tests/compression_streaming.py`。初次源变化注入修改了尚未读取的其他文件，未触发预期失败；已改为根据 entry 跟踪实际正在上传的文件后再修改，最终套件通过。

桌面测试过程：首轮与大文件测试并行时，在既有开关动画时序断言失败，尚未进入压缩用例；未修改玻璃实现，后续完整回归通过，未据此认定根因。新增压缩用例开发中曾在保存重置表单后立即读取选项而失败，已等待保存后的表单重置，再重新打开已保存目标验证。一次独立压缩验证遇到页面重载等待超时，新增用例的重载等待上限调整为 15 秒；随后独立验证与最终完整回归通过。

边界：压缩在 Server 的块写入阶段执行，网络上传仍为原文块；本轮未交付独立归档导入/导出和加密。不可压缩块额外占 45 字节框架头。未专门对压缩写入注入 ENOSPC，也未验证物理掉电；完整构建中的既有 ENOSPC 测试仍通过，不据此宣称覆盖了所有压缩故障条件。配置格式 7 和压缩格式 5 需要同时更新客户端与 Server。

## 2026-10-26 按课件统一压缩算法接口

执行者：Codex。保留此前未提交的压缩备份实现及用户提供的三份 PDF。范围为将算法迁入 `ICompressionAlgorithm` 的两个实现类，集中工厂选择；块封装、原文回退、校验、配置与磁盘编号继续使用现有约定，不增加新算法。接口对应《实验指导书-压缩解压》第 5、10 页；参数沿用项目的有界字节块，错误使用 `Error` 异常。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | C++ 编译、链接及 TypeScript/Vite 构建通过；17 个 Meson 套件全部通过，0 失败、0 跳过。压缩集成套件耗时 24.41 秒，大文件压缩套件耗时 479.64 秒 |
| `compression-test` | 完整构建中通过，0.94 秒。编译期检查抽象接口和虚析构；运行时直接通过基类引用调用两种实现，验证普通、单符号和最大块数据往返；空输入、超限输入、非法原始长度、未知算法均被拒绝 |
| 重构前格式兼容 | 修改前使用原有静态库采集两种算法的 100000 字节混合重复样本、4097 字节单符号样本及 11 字节原文回退样本；六个完整 `BC01` 块的 SHA-256 固定在单元测试中。重构后各摘要完全一致，且均能解压恢复原文 |
| 真实备份与还原 | `backup-compression` 通过，两种算法的内容存储仍分别为 1,335,324 / 596,513 字节，原未压缩内容为 4,993,999 字节。类型、硬链接、历史版本、策略保存及重启、损坏拒绝、源变化、断线和提交回执丢失检查通过 |
| 64 MiB / 1 GiB 有界处理 | 两种算法各完成 64 MiB、1 GiB 原文的压缩备份和 SHA-256 还原比较；1 GiB 样本存储仍为 Huffman 138,612,736 字节、LZ77 16,977,920 字节。四次运行中 Agent 峰值不超过 25,324 KiB，Server 不超过 23,228 KiB；64 MiB 到 1 GiB 的内存差满足既有断言 |
| 针对压缩流程的桌面验证 | 临时复用 `desktop_window_smoke.mjs` 的 Electron 启动、CDP 点击和清理设施，仅调用 `checkCompression`；通过两种算法选择、持久化、重载、1120×760 / 850×600 布局、真实压缩版本与自动解压还原。本轮没有重新运行完整玻璃交互和窗口回归，不沿用上轮结果作为本轮执行证据 |
| `meson test -C build compression-test --print-errorlogs`（加载项目 Qt 环境） | 修正测试编译警告后重新编译并链接，通过 1/1，耗时 0.63 秒，重编译无新增警告；未关闭警告选项 |
| `clang-format 18.1.8 --dry-run --Werror`、`git diff --check` | 本轮接口、工厂、实现类、块封装及单测的格式检查通过，差异检查通过 |

首次编译在测试中用临时 `QString` 实参接收工厂基类引用处出现 `-Wdangling-reference`；工厂返回静态只读对象，测试已改用具名 `QString` 并完成上述重编译补验。共享算法对象没有可变成员，树、字典、编码和解码缓冲仍为每次调用局部资源。

证据：`tests/compression_test.cpp` 的接口与固定摘要用例、`tests/backup_compression.py`、`tests/compression_streaming.py`、`tests/desktop_compression.mjs`；完整构建实际结果记录于本节，`build/meson-logs/testlog.txt` 当前保存最后一次单测补验。未验证的物理掉电和专门压缩 ENOSPC 边界沿用上一节，不据此扩大覆盖声明。

## 2026-10-26 压缩模块注释中文化

将压缩接口、工厂、Huffman/LZ77 实现、相关测试及新增的配置兼容说明注释改为中文，保留标识符和错误消息。`.clang-format` 设置 `FixNamespaceComments: false`，防止格式化时将中文命名空间注释替换为英文。

验证通过：9 个 C++ 文件去除注释后的词法序列一致；2 个 Python 文件去除文档字符串后的 AST 一致；桌面压缩测试去除注释后的 JavaScript 编译输出一致。C++ 格式检查、Prettier 检查及 `git diff --check` 通过。本次仅调整注释、文档字符串和格式选项，未重跑构建或功能测试。

## 2026-11-02 六类自定义备份与虚函数规则接口

执行者：Codex。按 FR-11、UC-08 和需求附录 B 补齐路径、类型、名字、修改时间、尺寸、UID 条件的新建、编辑、预览及历史快照。`IBackupFilter` 使用虚析构和纯虚 `category()` / `matches()`，六种实现由工厂创建，`SourceScope` 统一组合；扫描、上传、Server 条目校验及历史还原共用规则。注释使用中文。本轮未提交 Git。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh` | C++ 编译链接完成；首轮 19 个 Meson 套件中 18 通过、1 失败、0 跳过。失败为 `backup-file-type-mounts`：设备新增 UID/mtime 元数据后，清单仍按旧条件拒绝设备元数据；已扩展格式 4/5 的元数据范围校验并补验通过。首轮压缩大文件套件通过，525.49 秒；普通流式套件通过，121.32 秒。没有将这次完整脚本记录为全绿 |
| 加载 `scripts/qt-env.sh` 后 `ninja -C build` | 最终 C++ 编译及链接通过，未关闭警告选项，无新增编译警告。直接运行一次未加载 Qt 环境的 Ninja 曾因本地 libproxy 的间接依赖链接失败，加载项目环境后解决 |
| `meson test -C build backup-filter-test backup-custom-filter backup-integration backup-type-filter backup-sources backup-file-type-mounts backup-file-types backup-compression --print-errorlogs` | 最后补齐空目录发布/还原校验后，8/8 通过，0 失败、0 跳过；耗时依次为单测 0.03、设备挂载 2.80、基础集成 6.48、类型筛选 7.79、来源选择 8.34、自定义筛选 10.88、文件类型 14.84、压缩集成 18.33 秒。未再次运行完整大文件套件 |
| `backup-filter-test` | 直接通过基类指针执行六种实现；验证中文/换行文件名、大小写、`?`、路径通配不跨 `/`、目录子树、大小和时间闭区间、不同 UID 的同类或与排除优先、64 位尺寸、非路径目录排除及非法字段/数值/模式 |
| `backup-custom-filter` | 比较扫描样本、实际 `entries.jsonl` 和还原树，逐项比较入选内容摘要或链接/FIFO 类型；验证路径/名称/类型/范围组合、空版本、显式选择的父目录排除、只选一个硬链接成员、草稿预览不保存且独立记录、非法规则与写配置失败保留旧状态、旧迁移接口拒绝夹带条件、重启、格式 7 不降级、压缩组合及历史规则不变 |
| 空目录边界 | 符合条件的空目录及自身不匹配的结构父目录可备份还原；Server 拒绝只有不匹配空目录的提交。修改历史清单并重新生成有效摘要后，Agent 仍拒绝不匹配且没有入选后代的目录，在创建还原目标前失败 |
| `backup-file-type-mounts` | 私有命名空间中真实字符/块设备元数据备份及原有跨设备边界通过；新增类型 + 实际 UID + mtime 单点闭区间组合，扫描与清单只包含指定字符节点，不生成 `.data`，不读取设备流 |
| `npm run build --prefix apps/backup-desktop` | TypeScript 主进程、Renderer 及 Vite 构建通过；补齐新代码控制体花括号并格式化后再次构建通过 |
| `npm run test:window --prefix apps/backup-desktop` | 完整窗口回归通过，含既有玻璃、窗口恢复、来源/类型、版本删除和压缩流程，以及新六类页签、创建、编辑、草稿预览、无效负尺寸禁用保存、取消保留旧值、重载、真实备份及还原。1120×760、850×600 均无横向溢出，底部操作可见。随后新增的空目录服务端校验由上面的 8 套件补验，未重新跑完整窗口回归 |
| `npm run test:bridge --prefix apps/backup-desktop` | 超长响应、合并/分段响应、超时后迟到响应及后台工作存活检查通过 |
| 格式及差异 | clang-format 18.1.8 检查新接口/实现、SourceScope、状态、遍历器、记录、Agent 入口及单测；Manifest/Server/还原只格式化本轮修改行，保留无关代码。Prettier 检查新界面、IPC 和桌面测试通过；Python 语法检查及 `git diff --check` 通过 |

附录 B 的权限限制：当前账号 UID=1000，不能把 `sub/f.txt` 改属其他 UID。B-03、B-04、B-05、B-07、B-08、B-09 使用原预期验证；B-01/B-06 按 f.txt 的实际 UID 增加入选项，不能记为原附录配置完整通过。B-02 验证未出现 UID 的空结果，不代替 V 属主真文件的正向场景；不同 UID 的正向匹配、同类或、跨类且和排除由单测覆盖。设备挂载测试另验证实际设备属主值，但也不代替 B-02 普通文件样本。

测试开发中，首次集成用例误用 `manifest.jsonl`，已改为实际 `entries.jsonl`；桌面专项首次启动遇到并行链接时 Server 尚未就绪，重启专项后进入用例，后续又修正了测试从错误字段取版本编号的问题。修正后专项和最终完整桌面回归均通过，未改应用来绕过这些测试错误。

证据：`tests/backup_filter_test.cpp`、`tests/backup_custom_filter.py`、`tests/backup_file_type_mounts.py`、`tests/desktop_custom_filter.mjs`；本轮完整构建输出 `/tmp/backup-custom-filter-build.log`，最终八套件 `/tmp/backup-custom-filter-acceptance.log`，完整桌面 `/tmp/backup-custom-filter-window.log`。`build/meson-logs/testlog.txt` 可能被后续单项补验覆盖。截图为 `/tmp/backup-custom-filter-1120.png`、`/tmp/backup-custom-filter-850.png`，已查看窄窗截图确认内容滚动及操作区布局。

边界：实时备份尚未实现，本轮不宣称覆盖实时触发链路；未验证百万条目规模、物理掉电及独立筛选场景的 ENOSPC。配置持久化故障通过把 `config.json` 替换为不可写入的目录注入，证明失败保留内存和旧文件，不等同于磁盘写满。历史版本按当次规则还原，不按当前任务条件重新筛选。

## 2026-11-10 自定义备份混合目录实测（2026-11-12 收尾复核）

执行者：Codex。按用户要求创建实际测试目录，使用独立 Agent 配置和 Server 仓库，逐项核对扫描、已提交清单和真实还原文件。没有使用日常备份配置。本轮未提交 Git。

保留的证据目录为 `/home/jjj/course/cpp/backup-filter-acceptance-20261110-final`。其中 `source/` 含 32 个条目：多层目录、空目录、中文/空格/隐藏/换行名称、大小写扩展名、0/9/10/20/21 字节边界文件、1 MiB 二进制文件、不同修改时间、硬链接、文件/目录/悬空/循环软链接、FIFO 和作为应跳过样本的 socket。`source-inventory.json` 记录 UID、mtime、大小、模式、内容 SHA-256 或链接目标；`restored/` 保留逐用例还原目录；`results.json` 保留规则、预期、实际结果和版本 ID。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `./scripts/build.sh`（2026-11-10） | C++ 和桌面端构建成功；19/19 个 Meson 套件通过，0 失败、0 跳过、0 超时。包含自定义筛选、设备挂载、损坏/中断边界、版本删除以及大文件流式与压缩测试；压缩流式套件耗时 268.22 秒。日志已保留为证据目录中的 `full-build.log` |
| 加载 `scripts/qt-env.sh` 后运行 `python3 tests/backup_filter_acceptance.py build/backup-agent build/backup-server /home/jjj/course/cpp/backup-filter-acceptance-20261110-final` | 33 组筛选场景全部通过：路径子树及通配符不跨目录、类型、名字/大小写/中文/隐藏名称、尺寸闭区间与单边/零尺寸、mtime 闭区间与单边、UID、同类或、跨类且、排除优先、目录剪枝、无匹配和只选空目录。逐例比较扫描中未跳过的条目、真实 `entries.jsonl`、实际还原树和文件摘要/链接目标/FIFO 类型；硬链接组另核对还原 inode |
| Huffman / LZ77 与条件筛选组合 | 另外 2 组通过真实备份和还原核对；Agent 重启后保留已保存规则，旧版本仍按其历史规则还原 |
| `unshare --user --map-root-user --mount python3 tests/backup_filter_acceptance.py build/backup-agent build/backup-server /home/jjj/course/cpp/backup-filter-owner-20261110 --ownership` | 3 组实际不同 UID 的普通文件通过：指定其他属主、两个 UID 或组合、排除其他属主。私有命名空间内本地文件 UID=0，只读绑定的宿主 root 文件显示为 UID=65534；没有改变宿主文件属主。绑定测试结束后卸载，结果保留在 `ownership-results.json` 和 `ownership.log`。此场景不等同于宿主机 UID=1001 的文件 |
| 桌面专项 `checkCustomFilters`（2026-11-10；2026-11-12 再次执行收尾复核） | 复用完整窗口脚本的 Electron/CDP 启动与真实鼠标输入，仅运行自定义筛选专项。覆盖六类条件填写、创建、编辑、草稿预览不保存、重载持久化、取消不保存、真实备份还原、1120×760 / 850×600 布局。另覆盖跨页签空条件定位，以及 8 组非法路径、名字、尺寸、UID、时间范围，确认保存被禁用且修正后恢复。此次没有重新运行完整玻璃/窗口回归，不用专项结果代替完整窗口套件 |
| `npm run test:bridge --prefix apps/backup-desktop` | 超长 UTF-8 响应、分帧与后续请求、后台工作存活验证通过；日志为 `bridge.log` |
| 差异与语法 | `git diff --check`、新增 Python 脚本语法检查通过。自定义筛选组件、局部样式、桌面专项的 Prettier 检查通过 |

本轮共 38 组明确列出的筛选/压缩用例通过，另有重启历史检查与 GUI 验证。2026-11-12 核对了保留的 JSON 结果及全部主目录还原目录；没有将旧日期的完整构建写成当天重新运行。

实际发现及处理：

- 截图中的红字来自其他页签中未填写的条件，原提示没有标明位置。已在自定义备份卡片内显示类别、选入/排除组、组内序号，并提供“定位条件”按钮。回归先复现原笼统提示的失败，再验证修复后能定位名字条件，删除后能定位剩余尺寸条件；记录为 `gui-error-before.log` 与 `gui-after.log`。
- **原修改时间还原仍未实现。** mtime 用于筛选并正确保存在清单中，但本轮实际还原文件的修改时间是还原时刻。每个用例的 `restore_mtime_not_preserved` 保留差异；这项没有记为通过，不能宣称 FR-12 完整元数据还原已实现。本轮没有扩展修改后端元数据恢复流程。
- 自定义“类型=普通文件”采用单链接普通文件语义，源端链接数大于 1 的文件属于“硬链接成员”；两种类型可同时添加。测试按该区分分别核对，未把硬链接成员的未入选误记为内容丢失。

测试脚本开发时修正过两处预期：扫描样本包含带 `reason` 的已跳过 socket，不能将它计为入选文件；自定义类型中的普通文件与硬链接成员应分别计算。初次额外检查原 mtime 还原也失败，并按上面的实际未实现项记录，没有通过修改应用来掩盖。此前未完成的样本运行目录保留供诊断，最终结果以 `*-final` 为准。

## 2026-11-12 扫描预览改为可折叠目录树

执行者：Codex。按用户要求，仅调整扫描文件明细的展示：目录先于文件排列，默认展开第一层，子目录可分别展开/收起，支持全部展开/全部收起；名称列显示基本名称，悬停查看完整相对路径。保留类型、大小、SHA-256、链接目标、硬链接同组路径和设备号；统计、警告与扫描/备份选择逻辑未修改。

树由现有 `sample` 构建。缺失的父目录以“路径层级”标注，不增加统计。目录中没有返回的子项时显示“此目录没有预览子项”，不据此认定目录为空。说明中明确目录树只包含预览条目；本轮没有扩展后端预览分页，也没有把有限样本宣称为完整文件浏览器。软链接保持叶条目，不读取或展开其目标。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| `npm run build --prefix apps/backup-desktop` | TypeScript 主进程、Renderer 和 Vite 构建通过 |
| `node tests/scan_tree_test.mjs` | 父目录晚于子项、缺失父目录、目录优先及自然数字排序、层级折叠、元数据保留、软链接叶条目、`__proto__` 路径、空样本及 1500 层路径通过；迭代遍历无递归栈溢出，未修改输入顺序 |
| 真实窗口专项 `checkScanTree` | 创建包含多层目录、三个同名 `a.txt`、空目录、中文长名称和目录软链接的真实样本；实际扫描后验证逐层展开、父目录收起后子状态保留、全部展开/收起、SHA-256 完整值、重新扫描重置、软链接不展开，1120×760 和 850×600 无横向溢出 |
| 文件类型与自定义类型筛选窗口专项 | 与目录树专项同一真实窗口继续运行 `checkFileTypes` 和 `checkTypeFilter`，实际备份/还原、硬链接 inode、FIFO、规则持久化、直接选择类型不匹配及无匹配场景通过；原扁平路径断言改为读取行的完整 `data-path` |
| 格式与差异 | 新树模型、树表格组件、局部 CSS、相关测试的 Prettier 检查和 `git diff --check` 通过 |

此次没有修改 C++ 逻辑，没有重新运行完整后端套件。以上专项由临时脚本复用 `desktop_window_smoke.mjs` 的启动、CDP 点击及清理代码；`checkScanTree` 已接入完整窗口回归。专项日志和两种窗口截图保留在 `/home/jjj/course/cpp/backup-scan-tree-20261112/`。

接入时的回归问题：前两次完整窗口运行均在警告第一页加载处超时。最小复现确认平铺表格可加载，目录树版本中原生 `toggle` 事件也已触发，因此不是鼠标未点中。原因是 `WarningDetails` 与新增的 `ScanTreeTable` 两个相邻组件使用相同的执行 ID 作为 key，造成更新关联异常；已给目录树增加独立的 key 前缀，警告组件本身未修改。201 条警告的独立复现随后通过，新的目录树用例也加入实际 socket 跳过警告自动加载检查。

修复后执行 `npm run test:window --prefix apps/backup-desktop`：完整窗口回归通过（进程退出码 0），包含玻璃/窗口恢复、扫描警告 100 条分页、执行记录、备份还原、来源与类型选择、单版删除、Huffman/LZ77、自定义筛选，以及新增目录树与警告独立更新用例。最终日志为上述证据目录中的 `window-passed.log`；先前失败日志保留为 `window-first.log`、`window-second.log`。临时专项脚本已移除。

## 2026-11-12 硬链接关系说明文案

扫描目录树中的“同组文件：”改为“与此文件共享数据：”，同步更新现有桌面文件类型测试的文案预期。只修改显示文字，硬链接识别和存储还原逻辑保持原样。`npm run build --prefix apps/backup-desktop` 和 `git diff --check` 通过；未重复运行功能或完整窗口测试。

## 2026-11-17 FR-12 元数据恢复补齐

执行者：Codex。范围为当前 Linux 文件系统上普通文件、目录、硬链接、软链接、FIFO 的 rwx、UID/GID 和纳秒级 mtime 保存/恢复；软链接权限位、ACL、扩展属性、atime、ctime、特殊权限位及设备内容不在本轮承诺中。本轮未提交 Git。

先使用真实备份还原复现此前问题：17 字节普通文件的权限、UID/GID 正确，但 mtime 从样本设定值变成还原时刻。原因是 `restoreMetadata()` 后的 `syncFile()` 才刷新 Qt 写缓冲。现改为先受检刷新内容，再恢复元数据，随后同步和无覆盖发布。

其他补齐项：硬链接最终验证移到目录权限收紧之前；共享元数据校验器同时验证清单与根目录，明确记录冲突元数据的硬链接被拒绝，旧别名省略元数据时继承代表文件信息。元数据警告不再截断为 100 条，根目录失败与源版本警告共同保存，字段信息随历史记录持久化，已有分页接口读取完整明细。界面显示“内容已恢复，元数据未完全恢复”，警告与还原写入明细使用不同组件 key，能分别展开。

| 命令 / 验证项 | 实际结果 |
| --- | --- |
| 修复前 `backup_metadata.py` 实测 | 在 `small.txt` 的纳秒 mtime 比较处失败，保留日志为证据目录中的 `before.log` |
| `backup-metadata` | 未压缩、Huffman、LZ77 三种版本逐项比较根目录、嵌套/只读/空目录、普通/空/跨块文件、硬链接、外部/悬空软链接及 FIFO 的 rwx/UID/GID/mtime；含纳秒、纪元前时间和当前用户允许的其他属组。内容、链接 inode、链接目标和外部目标不变检查通过 |
| 受限目录 | 使用有效清单模拟目录最终权限为 0400，硬链接验证在收紧遍历权限前完成，还原成功；测试后仅为样本清理恢复其遍历权限 |
| 无元数据历史与非法清单 | 缺少元数据的格式 2/3/4 可读；11 组非法/不完整字段、根元数据或硬链接冲突在创建还原目标前拒绝。Server 另拒绝非法根元数据，未发布损坏版本 |
| 仅测试库注入权限失败 | 对目标文件、根目录、子目录、FIFO、软链接注入属主/权限/时间设置失败；326 项元数据警告与 1 项源 socket 跳过警告完整保留并分页，非目录内容仍一致，Agent 重启后计数及逐项明细不变。生产程序不含故障开关 |
| `backup-metadata-owners` | 私有 user/mount namespace 中只读绑定真实不同属主文件，无法映射的 UID/GID 实际恢复失败；结果为完成并有警告，明确列出 `foreign` 的 `owner/group`，内容及可恢复的权限/mtime 正确，宿主源文件元数据不变。没有把该场景当作宿主任意 UID 成功恢复的证据 |
| 桌面元数据专项 | 使用另一合法属主的版本元数据制造当前用户真实权限失败；验证还原内容、根目录和文件警告、明确提示、警告/写入明细独立展开、重新加载后查询，专项通过 |

首次完整构建中 19/21 个套件通过、2 个失败，失败为 `backup-file-types` 和 `backup-file-type-mounts`。它们的旧兼容清单允许代表文件有元数据、别名没有元数据；最初校验过严而提前拒绝。现保留该兼容，只拒绝别名明确记录的冲突元数据。没有删除原用例或降低其路径、类型、跨设备验证要求。

测试脚本开发中曾在生成格式 4 无元数据样本后误写回旧清单摘要，导致摘要校验失败；修正生成顺序后历史兼容检查通过。最初 `field` 在历史记录规范化时被丢弃，已补齐可选字段持久化并验证重启后的完整明细。

证据目录：`/home/jjj/course/cpp/backup-metadata-20261117-evidence/`；独立保留的样本与还原目录：`/home/jjj/course/cpp/backup-metadata-20261117-verified/`。`before.log`、`build-first.log`、`focused.log`、`verified.log`、`window-focused.log` 分别记录复现、首轮完整构建、新专项、独立实测和桌面专项。

本节补齐此前记录的普通文件修改时间问题；此前测试记录保留当时实际结果，不应再将其“尚未实现”作为当前结论。旧版本未记录的元数据无法补回，不自动提权；未验证物理掉电、任意文件系统、ACL/xattr 或百万条目规模。

最终验证补充：

| 命令 | 结果 |
| --- | --- |
| `./scripts/build.sh`（修正旧硬链接兼容后） | C++ 构建、21/21 个 Meson 套件及桌面端构建通过，0 失败、0 跳过、0 超时。1 GiB 压缩流式套件通过，265.65 秒。完整日志为 `build-passed.log` |
| 最后增加节点权限设置的 `AT_SYMLINK_NOFOLLOW` 后，加载 Qt 环境重新编译并运行 `meson test -C build backup-metadata backup-metadata-owners backup-file-types backup-file-type-mounts backup-integration backup-compression --print-errorlogs` | 最终二进制补验 8/8 通过，覆盖元数据、实际属主权限失败、字符/块设备挂载与跨设备边界、基础还原和两种压缩。没有把这次六套件写成再次运行完整 21 套件 |
| 加载 Qt 环境运行 `backup_filter_acceptance.py`，输出到 `/home/jjj/course/cpp/backup-filter-metadata-20261117` | 33 组筛选及 2 组压缩组合再次通过，普通文件和硬链接成员还原后的 mtime 改为强制断言，全部一致；重启后历史版本还原通过，结果记录 `mtime_verified: true` |
| 格式 / 语法 | clang-format 18.1.8 格式化本轮 C++ 修改行和新增故障库，保留无关旧代码；Black 检查两个新增 Python 专项，Python 语法、桌面改动 Prettier 和 `git diff --check` 通过 |

完整窗口回归首轮中，元数据用例及全部已有功能用例均通过，但最后最小化后的 500 毫秒后台计时器断言失败，因此该次完整窗口运行不记为通过。专项末尾会重载页面，已在窗口后台计时检查前等待现有 `requireGlass()` 确认页面渲染就绪；没有修改应用后台参数，也没有放宽 500 毫秒断言。首轮日志保留为 `window-first.log`。

第二次完整窗口运行增加窄窗几何检查时，直接比较面板 `scrollWidth/clientWidth`，误将玻璃按钮向外延伸的阴影画布视作内容溢出；诊断中唯一超界节点是该画布。已改为检查页面宽度、提示文字及原生控件的实际边界，未修改应用布局。1120×760、850×600 专项均通过，并已查看截图确认文字和操作可见。日志为 `window-layout-first.log`、`window-layout-passed.log`，截图为证据目录中的 `backup-metadata-1120.png`、`backup-metadata-850.png`。

最后执行 `npm run test:window --prefix apps/backup-desktop`：完整窗口回归通过，退出码 0。包含既有全部窗口/玻璃、扫描、备份还原、版本删除、压缩、自定义筛选和目录树场景，以及新增元数据真实属主失败、两种尺寸、完整警告/写入明细独立展开、重载持久化。后台 500 毫秒计时、窗口恢复和关闭检查通过；这不构成对首轮单次时序失败根因的证明。最终日志为 `window-passed.log`，临时专项脚本已移除。

## 2026-11-17 虚接口与模块职责重构

本节记录初版接口方案及当时结果；后续精简方案和本轮验证见下一节，当前模块结构以[接口说明](architecture-refactoring.md)为准。

依据 [huawei-coding](../.agents/skills/huawei-coding/SKILL.md) 检查自写模块，新增 `ISourceSink`、`IMetadataTarget`、`IRestoreNode`、`IAgentOperation` 四组接口；保留已有压缩及六类筛选接口，拆分遍历状态/条目编码，合并前端惰性分页 Hook。职责、生命周期与各模块的处理依据见[接口说明](architecture-refactoring.md)。

以本轮开始时的工作区为基线保存源码副本和 SHA-256，保留此前未提交的元数据功能。没有更改界面布局、协议格式、存储格式和功能需求，没有修改第三方玻璃渲染源码，也没有执行 Git 提交。

测试版本为 `42f5195` 上的本轮未提交工作区，包含此前的元数据补全。Agent/Server 从当前源码构建；窗口运行于本机 Wayland / SwiftShader，不能将结果写成 WSLg 宿主原生输入或其他平台已验收。

| 验证 | 实际结果 |
| --- | --- |
| 加载 Qt 环境编译，执行 `meson test -C build agent-strategy-test backup-metadata backup-metadata-owners backup-file-types backup-file-type-mounts backup-custom-filter backup-sources version-deletion --print-errorlogs` | 首轮 7/8 通过：既有七个集成套件通过，新增单元夹具遗漏 `metadata_warning_count` 初始化而失败。补齐夹具中接口约定的统计字段后，`agent-strategy-test` 单独通过；没有放宽生产校验 |
| `agent-strategy-test` | 经基类调用真实分块遍历和扫描任务，核对跨块内容、摘要、硬链接只发送一份内容及统计；验证新建目录/软链接/FIFO、拒绝覆盖与未知类型、来源/状态目录重叠拒绝；替代元数据目标验证属主→权限→时间顺序、纪元前纳秒时间、部分失败继续、字段完整及旧版本缺失信息跳过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过；超大 UTF-8 响应只影响自身请求，Agent、后台工作和后续请求仍可用 |
| `./scripts/build.sh` | 退出码 0；C++ 构建、22/22 个 Meson 套件、TypeScript 和 Vite 构建通过，0 失败、0 跳过、0 超时。1 GiB 压缩流式测试通过，276.69 秒；没有新增编译告警 |
| 补充真实查询命令工厂边界后重编 `agent-strategy-test` 并直接运行 | 退出码 0；`versions`、`ping`、`confirm` 及未知命令均留给查询入口，四种数据操作工厂仍可用。本次只增加测试断言，没有更改应用代码 |
| `npm run test:window --prefix apps/backup-desktop` | 完整窗口回归通过，退出码 0。覆盖既有玻璃/控件与窗口检查、扫描/备份/还原、来源与类型选择、删除、Huffman/LZ77、自定义筛选、目录树和元数据专项；201 条警告按 100/100/1 分页，无重复遗漏。真实属主失败、警告与写入明细独立展开、重载查询、500 毫秒后台计时和关闭检查通过 |
| 格式与静态检查 | 新模块和新测试通过 clang-format 18.1.8 `--dry-run --Werror`；前端三文件 Prettier、TypeScript/桌面构建及 `git diff --check` 通过；说明文档的相对链接存在。新增单元里字符串临时对象触发的 GCC 引用寿命告警改用具名参数消除，没有屏蔽告警 |

完整日志保留在 `/home/jjj/course/cpp/backup-engineering-20261117-evidence/`：`build.log`、`backend-testlog.txt`、`window.log`、`bridge.log`、`focused.log`、`unit-final.log` 分别记录完整构建/后端、窗口、桥接、首轮专项及修正后的单元结果；另有基线/最终源码哈希及专项截图。

接口测试不等于全路径覆盖，不据此宣称实现实时备份、加密、归档、远端 TLS，或完成物理掉电与任意文件系统验证。

## 2026-11-17 接口精简（当前方案）

按实际替换需求保留 `ICompressionAlgorithm`、`IBackupFilter`、`ISourceSink`，移除初版的 `IAgentOperation`、`IRestoreNode`、`IMetadataTarget`。任务使用普通函数分发及统一白名单，特殊节点使用函数和返回结构，元数据两条系统调用路径共用字段解析与警告汇总。独立模块、RAII、无覆盖发布、恢复顺序及错误语义继续保留，详见[当前接口说明](architecture-refactoring.md)。

本轮以 `/tmp/backup-before-interface-trim-dk3lyauq/` 的工作区快照为基线。保留的三组虚接口、来源遍历/编码模块及 Electron/React 源码哈希不变，先前未提交的元数据功能及界面修改继续保留；没有执行 Git 提交。原 `agent-strategy-test` 按当前职责更名为 `agent-module-test`，旧名称及初版窗口结果仅属于上一节的历史记录。

| 命令 / 检查 | 实际结果 |
| --- | --- |
| 加载 Qt 环境编译，并执行 `meson test -C build agent-module-test backup-metadata backup-metadata-owners backup-file-types backup-file-type-mounts backup-integration version-deletion --print-errorlogs` | 7/7 通过。覆盖实际元数据恢复/权限失败、设备挂载边界、基本备份还原、特殊节点及版本删除 |
| `agent-module-test` | 保留经虚接口观察真实分块读取、硬链接与扫描失败汇总；改为直接测试任务分类/分发、拒绝查询和未知数据命令、来源/状态目录重叠、节点创建及拒绝覆盖。元数据使用真实文件 fd 和软链接，核对权限、属主、纪元前纳秒 mtime、不跟随链接、坏 fd 的三字段完整失败以及历史缺失字段跳过 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过；超大 UTF-8 响应只影响该请求，后续请求与后台工作继续 |
| `./scripts/build.sh` | 退出码 0；精简后的 C++ 构建、22/22 个 Meson 套件、TypeScript 和 Vite 构建全部通过，0 失败、0 跳过、0 超时，无新增编译告警。1 GiB 压缩流式测试通过，288.53 秒 |
| 格式与引用 | 精简的头文件、实现、测试通过 clang-format 18.1.8 `--dry-run --Werror`，主程序/还原调用只格式化修改行；`git diff --check` 通过，当前说明文档的相对链接存在，生产代码已无三组被移除接口及旧工厂的引用 |

证据保留在 `/home/jjj/course/cpp/backup-interface-trim-20261117-evidence/`：完整构建、后端原始测试日志、首轮专项、桥接及编译日志，以及本轮开始前和最终源码哈希。改动范围核对只包含计划内的 11 个既有文件修改和 3 个模块/测试文件改名，其他基线文件保持不变。

本轮未重复完整窗口回归：Electron/React、IPC 字段和布局均未修改，上一次窗口回归结果保留在上一节，不作为精简后再次运行窗口测试的证据。变化的 C++ 路径由本轮模块及真实进程集成测试验证；当前机器以外的平台、物理掉电和百万条目仍未验收。

## 2026-11-17 独立归档导出与导入（FR-13）

测试基线为 `1db0a96` 上的本轮工作区。参考本地《打包解包技术》讲义第 2—4、10—13 页，以自有 BSAR1 实现完整版本单文件导出、导入新版本并复用还原。格式、课件对应、操作步骤和范围见[归档说明](archive.md)。本轮没有执行 Git 提交，也没有将 FR-15 加密、随机索引或增量归档计为完成。

| 验证 | 实际结果 |
| --- | --- |
| 第一轮普通文件/空文件真实进程验证 | 导出 `.bsar`、导入新版本、还原内容一致，空文件可读；运行工具时最初未加载用户级 Qt 环境导致进程无法启动，加载 `scripts/qt-env.sh` 后验证通过 |
| `archive-test` | none/Huffman/LZ77 内容以 37 字节记录切开压缩头和负载，增量解码和摘要正确，回调输出块不超过 256 KiB；重复 finish、完成后继续写入及中断回调被正确处理；补充拒绝旧版本格式与新节点配置冲突 |
| `backup-archive` | 原仓库停止并移走、来源移走后，三种源编码到三种目标策略的 9 种组合导入还原通过。空文件、空/只读目录、中文长路径、原始非 UTF-8 链接目标、外部/悬空链接、硬链接 inode 关系、FIFO、权限/UID/GID/纳秒 mtime 均核对；103 条警告跨页保留。历史 socket、带明确来源选择及自定义规则的版本、无条目版本、忽略不支持的 50 KiB 根属性扩展后保持实际元数据、重启后还原通过 |
| `backup-archive` 非法输入 | 27 组损坏/不完整归档拒绝，包括未知版本/标志/记录、超限声明长度、缺终止记录、追加尾字节、重复摘要、错置警告、内容损坏且重新计算外包摘要、组外硬链接、非法路径/属性、chown 哨兵 UID/GID 和压缩内容损坏；另拒绝缺失文件、目录、软链接及 FIFO 输入。失败后目标版本数不变，已有输出文件不被覆盖，来源/仓库/状态目录内导出被拒绝 |
| `archive-faults` | 导出断线清理临时文件；源归档在上传开始后被改写，已验证的私有快照仍正确导入；丢失提交回执通过同一 ID 查询且不重复发布；无法确认时 WAITING，恢复后确认；强制中断导入并重启 Agent，记录 INTERRUPTED，旧版本仍可还原 |
| `archive-disk-full` | 私有 user/mount namespace 的真实 1 MiB 导出空间和 2 MiB 仓库存储不足均触发 ENOSPC；无目标半成品归档/可还原半成品版本，导出临时文件清理，旧版本仍可还原。本机实际执行，没有跳过 |
| `./scripts/build.sh` | C++、27/27 个 Meson 套件、TypeScript 和 Vite 构建通过，退出码 0；0 失败、0 跳过、0 超时，无新增编译告警。新增归档流式套件通过，154.66 秒 |
| 最后补齐版本格式/节点配置一致性、元数据数值规范化及清单长度检查并完成格式整理后，重编并执行 `meson test -C build archive-test backup-archive archive-faults archive-disk-full backup-metadata backup-integration backup-file-types backup-file-type-mounts --print-errorlogs` | 最终二进制补验 8/8 通过；没有把补验记为第二次完整 27 套件运行 |
| `npm run test:archive --prefix apps/backup-desktop` | 真实 Electron/Wayland/SwiftShader 窗口专项通过，退出码 0：原生选择器取消、保存路径、拒绝覆盖、停原服务并移走原仓库/来源后导入另一压缩策略目标、从新版本还原、损坏提示、记录重载和关闭均通过；1120×760、850×600 无页面/表格横向溢出。已查看窄窗截图 |
| `npm run test:bridge --prefix apps/backup-desktop` | 通过；超大 UTF-8 响应不破坏后续请求和后台工作 |

大文件单独建立新进程，实际备份、导出、导入并还原 64 MiB+4 字节与 1 GiB+4 字节文件，内容 SHA-256 一致。来源内容文件在导出前删除，验证导出读取的是已提交版本；进程 VmHWM 观测如下：

| 文件规模 | Agent 峰值 KiB | Server 峰值 KiB |
| --- | ---: | ---: |
| 64 MiB | 25240 | 22724 |
| 1 GiB | 25448 | 22700 |

实际峰值没有随单文件长度线性增长；这不等于目录条目数量可无限增长，清单校验仍需与条目数相关的状态。

最初构造历史 socket 样本时，错误保留了格式 4 的元数据，并未同步允许的类型范围，旧校验器正确拒绝导出。已修正夹具为符合格式 3 的字段和保存范围，没有为通过测试而放宽原清单/范围校验。

窗口脚本首轮将被信号终止进程的 `exitCode === null` 误判为仍在运行；修正为同时查看 `signalCode`。第二轮所有业务断言通过，但主进程调试连接尚未断开，Electron 等待调试器而未退出；脚本在等待退出前关闭两条调试连接后，完整归档窗口专项通过。没有修改应用的退出行为或放宽业务断言。本轮运行归档窗口专项，未重复包含全部玻璃/其他功能的完整窗口脚本；对应后台功能由完整 Meson 回归覆盖。

格式使用 clang-format 18.1.8 整理新增 C++ 和既有文件修改行，Black 整理三个新 Python 脚本，Prettier 检查前端与窗口脚本，`git diff --check` 通过。当前源码与已提交旧代码的差异只属于本轮归档功能、共用版本校验及相关测试/文档。证据目录为 `/home/jjj/course/cpp/backup-archive-20261117-evidence/`，包含完整构建、专项与补验日志、内存观测、源码哈希及两种尺寸截图。

当前仅支持未加密 BSAR1 和已声明的 Linux 类型/元数据范围；未知加密标志拒绝，不宣称通过密码错误/认证加密验收，也未覆盖物理掉电、任意文件系统或百万条目规模。

## 2026-11-17 本地实时备份与常驻 Agent（FR-09）

在此前未提交的归档实现上继续开发；本轮没有执行 Git 提交。现有手动备份、压缩、元数据、筛选和归档逻辑复用，新增同用户 Unix socket 服务、GUI 控制代理、Linux inotify 监听及串行实时调度。使用方法、状态、开机边界见[实时备份说明](realtime.md)。

已实际完成的专项验证：

| 验证 | 实际结果 |
| --- | --- |
| `backup_realtime.py` 第一轮、溢出补充轮 | 首次自动备份、连续写入合并、子目录新增、重命名、删除，版本还原内容正确；关闭代理后修改，重开 PID 相同且产生新版本；Server 停止显示 RETRYING，恢复连接后补齐；SIGKILL Agent 后重启补扫；关闭开关后不再自动备份。第二轮用 SIGSTOP 暂停 Agent 后制造超过主机 inotify 队列容量的连续重命名，实际观察 overflow_count 增加且还原包含溢出后的文件 |
| `backup_realtime_faults.py` | 代理延迟已提交版本的响应，等待期间新增文件，随后自动产生包含新文件的版本；丢弃提交和确认响应进入 WAITING，关闭再开启仍暂停，不创建新操作；放通并确认同一操作后处理积累变化。最初测试在上传中新增文件，却错误期待两次成功；实际正确拒绝源目录变化并重试，因此将“成功后的排队”场景改为延迟提交响应后验证 |
| `node tests/desktop_realtime_window.mjs` | 真实 Electron 窗口点击实时复选框，初次备份后显示监控中；关闭整个 GUI 后修改文件，再开连接相同 PID，查看后台记录，并实际还原修改后的内容；关闭开关成功。850×600 无横向溢出，已查看截图 |
| `npm run test:archive --prefix apps/backup-desktop` | 切换常驻代理后，原有归档窗口专项仍通过，包括原生选择器、导出、原仓库不可用后的导入/还原、损坏拒绝、记录重载、1120/850 布局和退出。测试结束额外清理自己的常驻服务 |
| `npm run test:bridge --prefix apps/backup-desktop` | UTF-8 超大答复、迟到答复、分帧与后续请求通过；仅失败对应请求，后台执行不被取消 |

完整构建和最终补验结果在本节后续记录，不把专项通过代替未执行的场景。未执行开机自启安装、百万路径规模、跨用户攻击和物理掉电；本轮不包含远端认证/加密解锁。明确选定来源本身缺失时等待恢复，不发布空成功版本。恢复单文件原子替换监听和实时筛选排除已加入完整回归用例。


最终记录：

- `./scripts/build.sh`：C++ 构建、29/29 个 Meson 套件、TypeScript 和 Vite 构建通过；0 失败、0 跳过、0 超时，无新增编译告警。`backup-realtime` 32.83 秒、`realtime-faults` 9.27 秒；新增单文件原子替换、自动版本筛选排除与真实队列溢出用例均执行。
- 完整回归后补齐控制通道销毁前断开回调、开启任务时合并时间起点，以及通过固定节点描述符监听以拒绝来源父目录软链接替换。重新编译后执行 `meson test -C build backup-realtime realtime-faults --print-errorlogs`：最终二进制 2/2 通过（34.74 秒、9.52 秒）。未将专项补验记为第二次完整回归。
- 修改文件通过 clang-format 18.1.8、TypeScript/窗口脚本 Prettier、Python Black 和 `git diff --check`。界面只在任务卡新增开关/状态，既有窗口脚本仅补充其隔离 Agent 的清理，没有据此宣称重跑完整历史窗口套件。
- 日志和已查看截图保存在 `/home/jjj/course/cpp/backup-realtime-20261117-evidence/`，包括 `full-build.log`、`final-focused.log`、`window.log`、`archive-window.log`、`bridge.log` 和 `realtime-850.png`。
