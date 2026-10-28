# W2-B -- 候选交互与产品输入闭环

起点 HEAD：`f3538b4ef0699be1d86733977c493d67e295a513`（W2-A-R 交接）。
本轮代码提交：`c884e3a`（源码；运行包 `0.2.2-w2b` 由该提交构建）。
文档提交在本报告所在提交中；没有 push/tag/amend/reset --hard/clean。
仍为**内部运行包**：未注册、未切换系统输入法、没有已授权隔离环境。

## 1. 环境（脱敏）与隔离确认

| 项 | 观测 |
| --- | --- |
| OS | Windows 11 x64 build 26200（26200.9711）；Lenovo 物理机；没有已授权的专用 Windows 测试环境/VM |
| 工具链 | VS 2022 Professional + 内置 CMake + MSVC 19.44（x64，静态 CRT） |
| 引擎/数据 | 既有英文 `rime.dll`（librime `33e7814`）、W1 产品词表与 schema；未重建引擎 |
| 注册 | **无**。未写注册表、未注册 CLSID、未切换输入法 |
| 构建/运行位置 | `build/windows-product/`（OneDrive 同步卷，I/O 抖动风险仍存在） |

只读核查结论：没有已授权隔离环境。因此没有 Layer C（正式注册 + StrictTsf 真实前台 + 真实应用）。
最小自动化环境方案与一次性授权请求见 §10。

## 2. 第一提交门：剩余输入可靠性

### 2.1 1024 UTF-16 上限（真实 backlog）

源码：`platforms/windows/tsf_product/key_admission.h`（`RawTextRun`）、
`text_service_controller.cc`（`CaptureRawText` / `CancelRawSuffix` / `FlushRaw`）。

- 事件在 `kRawCapacity` 之前已被吃下；捕获文本保存在控制器内存中的
  `RawTextRun`（未提交块），已提交块保存在协调器的单个在途工作项中。
- 追加是**原子**的：整键不拆分，代理项对不会被容量边界截断。块满时先 `FlushRaw`
  把整块交给协调器，再让下一个键开启新块。只有当前块无法交出（前一引擎键/块未解决）
  且新键放不下时，后续键才继续被吃下并显式记为 `raw_dropped_keys` / `failed_raw_capacity`。
- 尚未接受的事件：仍在准入队列 FIFO 头部/尾部，协调器恢复后按序处理；不会越过在途项。
- 顺序：撤出/分块/提交都保持 FIFO；块按提交顺序应用；没有重放。
- 避免重复：每次原文提交只有一个 `work_id`；应用后清零，绑定退役重建；同一块不会再次提交。
- 测试实际触及边界：`product.integration.sink_capacity` 在**文档锁被持有**的情况下先让首个
  引擎键在途，再发送 1200 字符；实测 `raw_unrecoverable > 0`、`raw_dropped_keys > 0`、
  `raw_applied_units == 1024`、文档保留前缀等于键入前缀、长度 1024/1025、无重复写入。
  边界后的**同会话继续输入**未通过（见 §9 门 2）；当前测试用**新激活**验证恢复：
  `capacity: a fresh activation becomes ready after the boundary loss` 与
  `capacity: the fresh session's document is exact` 均通过。

### 2.2 降级期控制键

源码：`HandleRawKey` / `CancelRawSuffix`；单测 `RawTextRun`。

| 键 | 语义 |
| --- | --- |
| Space | 追加 U+0020 并结束该段；按原文提交 |
| Enter | 结束该段并提交已捕获后缀；不插入换行 |
| Escape | **取消尚未提交的后缀**（计入 `raw_cancelled_keys/units`）；已交给协调器的块不撤回，按提交时一次生效；取消后不会迟到写入 |
| BackSpace | 只回退未提交后缀的一个 code point，代理项对整体回退；后缀为空记为 `raw_ignored_edits`；从不删除已显示组合、已提交文本或宿主既有文本 |

单测覆盖：容量前/边界/越界、代理项对不拆分、BackSpace 整对回退、取消记账。
端到端 B 覆盖了候选菜单下 Enter/Space/Escape 的文档结果；降级期 Enter/Escape 的
端到端用例仍未单独建立（§9 门 3）。

### 2.3 连续输入

`product.integration.sink_continuous`：只在整段开始前 `ReadyToType` 一次；首个 36 字符段
在**所有 TSF 授权被持有**时发送（强制真实 raw catch-up），随后 15 个词连续发送，
词间只 `PumpSlice(2)`（不是 WaitReady）；最终文档与独立 `ReferenceDoc`（键入文本本身）
**完全相等**，300 ms 后无迟到写入，之后引擎路径仍可用。测试通过。

### 2.4 激活等待（最长 60 s）

定位：`EngineStartTransaction::Prepare` 的 `WaitForSingleObject(mutex, 60000)` 与
`AcquireSharedLock(..., 60000)`，以及 `EngineHostBroker::Acquire` 内的部署；原先全部在
`EpoCanvaTextService::ActivateInternal` 的 STA 调用链上。

修复：`platforms/windows/product/product_engine_bootstrap.*` 把 `Prepare → Acquire →
Complete → TakeLeaseLock` 整体移到后台线程；`Activate` 立即以字面模式返回。
STA 在 `ProductTextController::PumpNow` 中采纳租约并构造 observer/coordinator。
`Deactivate` 不 join 后台启动，也不在 STA 释放可能阻塞的租约（`ReleaseAttachmentAsync`，
持有模块引用）。字面模式每个键保持宿主默认效果；`HostTokenOwnership` 防止 attach 时
接管半个 token。启动失败/未完成由 `engine_state/engine_failure` 与 `DllCanUnloadNow`
报告；下一次激活重试。

`product.integration.sink_startup` 使用受控阻塞启动验证：Activate < 1.5 s、启动中
`engine_state=starting`、所有键直通且文档精确、Deactivate < 1.5 s、启动线程阻塞期间
`DllCanUnloadNow == S_FALSE`、释放后状态 failed 且 `S_OK`。通过。
`product.integration.sink_engine_missing` 验证 rime.dll 缺失时激活仍成功、异步启动报告
failed、字面路径精确、marker 保留、无编译产物、停用不等待。通过。

## 3. 候选交互

源码：
- `platforms/windows/tsf_coordinator/candidate_snapshot.h`：快照/分类/请求类型。
- `coordinator.cc`：`UpdateCandidateSnapshot`、`InvalidateCandidateSnapshot`、
  `SubmitCandidateSelection`；每次引擎观察递增版本；绑定失效/原文提交/关闭清空。
- `text_service_controller.cc`：方向键选择、Enter（有显式选择才走校验选择）、Escape
  （关窗 + 引擎取消）、鼠标回调、`FinalizeCandidateSelection`。
- `candidate_ui.*` / `candidate_window.*`：真实 UIElement 与窗口。

实现结果：
- 原文/补全/拼写建议分别标记 `RAW/COMP/SPELL`（分类规则见
  `docs/windows/candidate-ui-contract.md`）；初始没有显式选择，建议不会被自动接受。
- 方向键循环选择；鼠标点击走同一校验路径；Enter 有显式选择时提交该索引，否则保持原
  Enter 契约；Space 不拦截，仍提交原文 + U+0020；Escape 关闭并取消组合。
- 每页 8 条，页大小来自引擎；产品 schema 的最大菜单是 1 原文 + 最多 7 补全
  （或 1 原文 + 最多 3 拼写建议），因此当前一页即末页。
- 选择请求携带快照版本、**快照自身**的 context/generation、页内索引；协调器逐项校验，
  过期/焦点变化/外部编辑/raw/绑定重建后的请求返回 `kCandidateStale`，不派发引擎命令。
- 窗口不调用引擎、不 `SetText`；提交只走产品控制器与既有协调器。

B 层证据 `product.integration.sink_candidate`（通过）：真实引擎菜单、页大小 8、索引 0
原文/索引 1 补全；无选择 Enter 提交原文；方向键 + Enter 提交选中的补全；Space 在有选择时
仍提交原文；Escape 取消组合；扩展组合后旧版本点击被拒绝且文档不变；外部编辑后旧版本点击
被拒绝；拼写建议被标为 `kSpelling`。

## 4. TSF UIElement 与窗口

源码：`candidate_ui.*`（`ITfCandidateListUIElementBehavior`、`ITfUIElementSink`、
`CandidateUi`）与 `candidate_window.*`（Win32 自绘）。

- `BeginUIElement` / `UpdateUIElement` / `EndUIElement` 生命周期；`pbShow=FALSE` 时
  **不创建**自绘窗口；宿主 End 会隐藏窗口并使后续 End 幂等；`EndUIElement` 回调期间
  不释放元素引用。
- 位置来自真实 TSF 范围：只读编辑会话 + `ITfRangeACP::SetExtent` + `ITfContextView::GetTextExt`；
  无布局/零矩形/不可见/无焦点 → 隐藏；不使用鼠标位置或屏幕角落。
- 窗口 `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`，`WM_MOUSEACTIVATE →
  MA_NOACTIVATE`；不调用 `SetForegroundWindow`/`SetFocus`；DPI 变化重建字体并重定位；
  按监视器工作区夹取，底部无空间时翻到上方。
- 未引入大型 UI 框架，复用 DLL 自身 HINSTANCE/GDI/Win32。

受控证据 `product.ui.candidate_window`（通过）：真实窗口绘制、8 行、点击索引、
`MA_NOACTIVATE`、截图；真实 `ITfCandidateListUIElement` 在假管理器下的
Begin/Update/End、宿主绘制抑制、宿主 End、选择/Finalize 回调携带版本与 identity；
100/150/200% 度量与多显示器/小工作区纯布局。截图：
`build/windows-product/product/run/candidate-ui/candidate-window.bmp`（140×206 RGB）。

## 5. 设置与可见状态

- 设置窗口的候选说明改为真实状态：原生候选窗口、每页 8 条、方向键/Enter/Space/Esc 语义、
  三类行标记。`product_diagnostics` 的 capabilities 同步更新。
- 只暴露已实现的设置（补全开关/数量、拼写开关/数量）；页大小由 schema 固定，未新增布局选项；
  保存/回读/生效时机沿用 W2-A 声明（下一次引擎启动）。
- 状态区分：`ControllerDiagnostics::engine_state`（idle/starting/ready/failed）、
  `engine_ready`、`literal_key_downs`、`candidate_menu_open/version/items/selected`、
  `candidate_ui`。`package=ready` 只说明文件完整性，不说明输入法工作正常。

## 6. 验证分层

| 层 | 结果 |
| --- | --- |
| A 纯类型/参考模型 | `product.unit.key_admission` 通过：高水位/FIFO、raw 记账、RawTextRun 容量/代理项/回退/取消；测试侧独立 `ReferenceDoc`。 |
| B 受控 TextStore + 直接 sink | 全部通过：`sink_raw`、`sink_token`、`sink_pressure`、`sink_candidate`、`sink_continuous`、`sink_capacity`、`sink_startup`、`sink_engine_missing`、`sink_focus_switch`、`sink_multi`、`sink_settings_timing`、`sink_lease_lock`；以及 `product.ui.candidate_window`。 |
| C 正式注册 + StrictTsf + 真实应用 | **未执行**：没有已授权隔离环境；未注册。Lab fallback 只用于 B。 |

`product.integration.sink_strict_unregistered` 仍只证明“未注册的严格 DLL 不能激活”，不是 C。

候选测试覆盖（B）：点击索引、键盘导航、过期点击、外部编辑、降级/恢复、关闭/卸载路径、
100/150/200% 纯布局、多显示器与小工作区；**未覆盖**：真实 TSF 前台的点击不抢焦点、
真实窗口在 150/200% DPI 下的像素级外观、真实多显示器边界（环境不具备）。

## 7. 构建、测试与实际命令

| 命令 | 结果 |
| --- | --- |
| `powershell -File tools/product/build_product.ps1 -SkipTests` | 0 |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error` | 0；143 项：**142 通过，0 失败，1 跳过**（`windows.tsf_context_observer.real_focus`，无真实前台焦点） |
| `stage_product.ps1 -Replace` | 0；`EpoCanva-0.2.2-w2b`，21 files，4262145 bytes；commit `c884e3a` |
| `EpoCanva.exe --diagnose <report>` | 0；`package=ready`、`manifest=ok`、21/21 匹配、`version=0.2.2-w2b`、`commit=c884e3a`、`tsf_registration=no` |

完整日志：`build/w2b-ctest-c884e3a.log`（未覆盖旧日志）。失败日志无（最终运行 0 失败）。
一次中间运行出现过 `sink_settings_timing` 的异步停止竞态；已改为在重启前有界等待
`EngineHostBroker` 回到 idle，随后通过；没有放宽任何产品语义。

## 8. 变更文件

新增：
- `platforms/windows/product/product_engine_bootstrap.{h,cc}`
- `platforms/windows/product/tests/candidate_ui_tests.cc`
- `platforms/windows/tsf_coordinator/candidate_snapshot.h`
- `platforms/windows/tsf_product/candidate_ui.{h,cc}`
- `platforms/windows/tsf_product/candidate_window.{h,cc}`
- `platforms/windows/tsf_product/product_test_seam.{h,cc}`（仅直接 sink 变体导出）
- `docs/windows/candidate-ui-contract.md`、本报告

修改：
- `platforms/windows/product/{CMakeLists.txt,VERSION,product_diagnostics.cc,product_engine_config.{h,cc}}`
- `platforms/windows/tsf/text_service.cc`
- `platforms/windows/tsf_coordinator/{coordinator.h,coordinator.cc}`
- `platforms/windows/tsf_product/{key_admission.h,key_admission.cc,sta_result_waker.h,sta_result_waker.cc,text_service_controller.h,text_service_controller.cc,tests/key_admission_tests.cc}`
- `tests/windows/tsf_product/product_sink_tests.cc`
- `docs/windows/tsf-keyboard-input-contract.md`、`docs/reports/W1-windows-alpha-product.md`（状态注记）、`README.md`

## 9. 未完成、风险与交付门

1. **Layer C 未验证**：没有已授权隔离环境；正式注册 DLL 在真实应用/StrictTsf 前台下的
   候选窗口显示、点击不抢焦点、DPI/多显示器未验证。需要一次性环境授权后由 W3 完成。
2. **容量边界后的同会话继续输入**：在受控主机中，1024 单元块应用后**继续输入新 token**
   触发 `textinputframework.dll` 访问冲突（同一偏移 0x270a5）。容量测试因此用**新激活**
   验证恢复；同会话恢复未声称安全。这是未关闭的输入可靠性门，根因未定位（故障模块是
   Windows TSF，不是产品 DLL）；W3/隔离环境应优先复现。
3. **降级期控制键的端到端覆盖**：Escape 取消/Enter 结束/BackSpace 后缀回退有单测与代码，
   但没有独立的 B 层降级用例；当前端到端覆盖集中在候选菜单与容量路径。
4. **真实 UIElement 管理器**：直接 sink 变体不创建 TSF UI 管理器（受控主机上的真实管理器
   路径曾崩溃）；UIElement 生命周期用假管理器验证，真实管理器留给 Layer C。
5. **窗口位置**：`ReadCompositionRect` 已实现但未在真实 TSF view 上执行；测试用合成 anchor。
6. **持续连续输入**：`sink_continuous` 通过；更长/更快的同会话连续输入未再扩展（受门 2 影响）。
7. **激活等待**：STA 已不承受 60 s；后台启动线程仍会等待命名互斥量/共享锁（有界 60 s），
   这是有意的。Deactivate 不 join 启动；下一次激活可能加入仍在停止的旧引擎，设置部署
   继续推迟到真正的新启动（设置文案“下一次引擎启动”仍准确）。
8. **运行包/公开分发**：`rime.dll` 内嵌构建路径仍为 67/68 处；词表再分发未决；
   `/pathmap` 重建与 legal 决定仍是 W3/公开分发门。
9. **OneDrive I/O 抖动**：全量运行位于同步卷；最终一次 143 项通过，但环境风险仍在。

## 10. 隔离环境请求（ Layer C ）

没有已授权环境，未在用户主系统注册/切换输入法，未启用虚拟化/下载镜像/关闭安全功能。
需要一次性授权的最小方案（W3 执行）：
- 一台可回滚的 Windows 11 x64 VM 或专用测试机（可注册/卸载 CLSID，可切换输入法）；
- 固定测试应用（记事本、浏览器文本框、VS Code 或等价）+ StrictTsf 前台；
- `regsvr32`/TSF 注册与卸载脚本、注册表快照与回滚点；
- 100%/150%/200% DPI、双显示器、屏幕边缘/小工作区；
- 全程只用固定测试数据；生产日志不含输入文本。
授权前不进行任何注册或系统级修改。

## 11. 交付状态

- 代码提交：`c884e3a`（完整 SHA 见交付消息；本报告的文档提交紧随其后）。
- 运行包：`build/product/stage/EpoCanva-0.2.2-w2b/`；身份 `0.2.2-w2b` + `c884e3a`；
  `--diagnose` 实际退出码 0。
- 真实窗口截图：`build/windows-product/product/run/candidate-ui/candidate-window.bmp`。
- 结束后停止，不进入 W3 的注册/安装器工作。
