# 候选窗口与 TSF UIElement 契约（W2-B，W2-C 改选择语义）

本文件只写 **已实现**的候选交互。它不是任务书，也不替代 `AGENTS.md` 的永久规则。
TSF 接口以磁盘上的 `msctf.h`（Windows SDK 10.0.26100.0）为准。

W2-C 只改一件事：候选选择不再是引擎命令，而是一次经过范围校验的**原文替换**；
原文在建议期间始终属于宿主。其余 UIElement 生命周期、分类、页大小与窗口规则未变。

## 1. 数据来源与快照版本

- 每次引擎观察（按键派发或候选接受）把引擎当前页复制成一份 `CandidateSnapshot`
  （`platforms/windows/tsf_coordinator/candidate_snapshot.h`）：页号、页大小、是否末页、
  页内文本与 comment，以及**该观察的绑定 identity/generation**。
- 每次观察都递增 `version`，包括清空菜单的观察；绑定失效、原文提交被接受、协调器关闭、
  NeedsResync/Uncertain 也会清空并递增版本。
- 文本只在内存中；快照、诊断和日志都不保存候选文本，生产日志不记录用户输入。

## 2. 分类

产品 Translator 固定输出：候选 0 是原文，然后是补全；补全存在时不生成拼写建议，
否则追加带固定 comment `spell` 的拼写建议。C API 只暴露 text/comment，因此分类规则是：

| 条件 | 类别 |
| --- | --- |
| `page_no == 0 && page_index == 0` | `kLiteral`（原文） |
| comment == `spell` | `kSpelling`（拼写建议） |
| 其余 | `kCompletion`（补全） |

窗口用 `RAW` / `COMP` / `SPELL` 三种标签显示，原文行不是建议，初始不被当作已选择项。

## 3. 选择校验

UI 只发起请求，不调用引擎、不 `SetText`。请求携带：

- `snapshot_version`
- **快照自身的** `context_id` 与 `generation`
- 页内索引 `page_index`
- **该 token 的已验证结束 caret ACP**（UTF-16 code-unit 偏移）

W3-A 起，caret 是菜单身份的一部分，但**不是**“当前光标”。控制器在 token 的第一个
字符通过宿主之前记录 token 起点（来自已确认的宿主 caret 投影），菜单锚点只接受
`token 起点 + 该 token 的宿主 UTF-16 单元数`。快照 caret 可能落后宿主最后一次编辑通知
（实测 8→9），所以展示和选择时会用只读 `GetSelection` 读取真实 caret，但只有它等于
上述 token 结束位置时才接受；任意其他 caret 都视为另一个 token，即使它前面恰好是相同
文本。选择请求携带这个锚点；协调器只接受 `caret >= input 长度` 的位置，真正的等价校验
在写入会话内。锚点更新**不会**扩大写入授权：`CompositionWriter::ReplaceToken` 仍要求
live caret 等于计划 caret，并逐单元比较 token 文本。

W2-C 起，选择**不再是引擎命令**，而是一次显式的原文替换
（`ContextCoordinator::SubmitSuggestionReplacement`）。只有以下全部成立才继续：
快照有效、版本一致、快照 identity 与请求一致、当前绑定 identity 与请求一致、
索引在该页内、位置不早于 token 起点。否则返回 `kCandidateStale`，计入
`candidate_stale_refusals`，不提交任何编辑。

真正的写入由 `CompositionWriter::ReplaceToken` 完成：它读取折叠 caret，要求
**live caret == 计划中的菜单 caret**，取 caret 前 `expected` 长度的范围，逐单元读回并
比较；caret 移动、文本不同、范围越界、选择非折叠、前一个字符仍是 token 字符、替换文本
为空都会**拒绝且不调用 SetText**。因此：

- 光标在菜单展示后移到别处 → 拒绝，即使新位置前面恰好是相同字符串；
- 相同字符串出现在文档其他位置 → 不会按文本相等就替换；
- 外部编辑、焦点变化、绑定重建后的旧点击 → 拒绝，原文保留。

“相同字符串”只有在**菜单展示时锚定的同一 caret** 且该范围逐单元等于引擎输入时才会被
替换；位置不可确认时失败关闭。

## 4. 键盘与鼠标

| 输入 | 行为 |
| --- | --- |
| ↑ / ↓（无修饰） | 菜单打开时被吃掉，循环移动显式选择；初始从“无选择”进入原文/末项 |
| Enter | 有显式选择 → 走第 3 节的显式替换；无显式选择 → **直通宿主**（宿主插入换行）；不自动接受建议 |
| Space | 不拦截，宿主插入自己的空格；菜单关闭，选择不生效 |
| Escape | 关闭菜单/清空选择；若引擎仍在组合，向**引擎流**发送取消（不写文档） |
| 鼠标点击 | 只回调“接受该页内索引”，与 Enter 走同一校验路径；窗口 `WS_EX_NOACTIVATE`，点击不抢焦点 |

不增加数字候选快捷键，不自动插入空格，不自动接受建议，不改变技术 token 的原文提交。

## 5. TSF UIElement 生命周期

`CandidateUi`（`platforms/windows/tsf_product/candidate_ui.*`）：

- 注册身份：CLSID 除 `GUID_TFCAT_TIP_KEYBOARD` 外**必须**实现
  `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`（`{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}`）。
  TSF 的 UILess 文档要求使用 `ITfUIElementMgr` 的 TIP 以此类别注册，宿主才能统一控制
  可见性；注册脚本与状态捕获都检查这一项。
- 通过 `ITfUIElementMgr::BeginUIElement` 注册每个候选菜单；`UpdateUIElement` 更新；
  `EndUIElement` 结束。元素实现 `ITfCandidateListUIElementBehavior`，提供
  `GetCount/GetString/GetSelection/GetPageIndex/GetCurrentPage` 与
  `SetSelection/Finalize/Abort`；`SetSelection/Finalize` 只回调控制器。
- `pbShow` 的初值由 TIP 写为 `TRUE`；`BeginUIElement` 返回后读取宿主写回的决定。
  按 SDK/官方文档，`TRUE` 表示宿主不绘制、由 TIP 绘制；`FALSE` 表示宿主接管绘制、
  TIP 不应再画。为 `FALSE` 时默认**不创建、不显示**任何自绘窗口（没有进程名/文件名例外），
  同时保留真实 `ITfUIElement` 并回答所有查询。按 UILess 契约，`BeginUIElement` 返回
  `FALSE` 后必须立即调用一次 `UpdateUIElement`，首次更新带全量标志
  （`DOCUMENTMGR|COUNT|SELECTION|STRING|PAGEINDEX|CURRENTPAGE`）。
- **W3-B 用户回退设置**（`candidate_display=host|always_self`，设置 EXE 写入
  `settings.ini`；由引擎启动事务在 bootstrap 线程读取并发布，下一次引擎启动生效；
  若本进程附着到其他进程已启动的引擎，则在本次激活读取一次）：`host` 为默认，严格遵从
  `pbShow`；`always_self` 在宿主返回 `FALSE` 时仍显示 TIP 窗口，用于“宿主接管但什么都没画”
  的应用。界面文案明确警告：宿主若同时绘制会出现两个窗口。显式 `Show(FALSE)` **始终优先于**
  `always_self`，隐藏后不因数据更新重开，只有宿主 `Show(TRUE)` 才交回绘制权。
  该设置是用户偏好，不是宿主白名单，不按进程名/文件名分支。
- **W3-B 态 C**：宿主根本没有 `ITfUIElementMgr`（`ITfUIElementMgr` QI 失败）时，TSF 允许
  TIP 自绘；控制器仍只使用真实 `GetTextExt` token 矩形作为锚点，无布局时关闭菜单而不是
  伪造位置。`EPOCANVA_TSF_DIRECT_SINK_TEST` 受控变体不进入态 C 自绘（它不是真实宿主）。
- `ITfUIElement::Show(FALSE)` 隐藏真实窗口并使 `IsShown` 返回 `FALSE`；后续数据更新不得
  重新打开。只有宿主显式 `Show(TRUE)` 才把绘制权交回 TIP，控制器在下一个节流唤醒重新
  读取 anchor 后展示。
- 没有显式选择时 `GetSelection` 返回 `S_FALSE`（`*puIndex` 无效）；`Finalize` 不会因此
  隐式接受原文行。显式选择时返回 `S_OK` 与页内索引。
- `ITfUIElementSink` 通过 `ITfUIElementMgr` 的 `ITfSource` 订阅；管理器发出的 End 会隐藏
  窗口并使后续 End 幂等。宿主自己的 `ITfUIElementSink` 是独立订阅；本 TIP 的 sink 不修改
  `pbShow`，也不替宿主做绘制决定。`EndUIElement` 回调期间不释放元素引用（元素在管理器
  调用返回后才释放），避免 TSF 在回调栈上持有已释放对象。
- 停止/卸载：`CandidateUi::Stop` 先 End、再注销 sink、释放管理器与窗口；桥接对象在停止时
  置空，迟到回调只能看到 inactive。

**已知宿主限制（证据，不是白名单）**：W3-A trace 轮次在 Windows 11 新 Notepad 中观测到
`BeginUIElement` 返回 `pbShow=FALSE`，且该宿主没有绘制候选列表；当时 stage8 在所有
`FALSE` 上自绘，属于违反 UILess 契约的临时诊断路径，已删除。正式链路默认严格按宿主决定
隐藏自绘窗口；W3-B 增加了**用户显式**的 `candidate_display=always_self` 回退，并如实警告
可能与宿主重复绘制。Notepad 是否自绘必须由正式包的 trace/diagnose 重新观测并记录。证据文件：
`build/w3a-c-local/evidence6/trace-hostdrawn-before.txt`（`ui_host_drawn|1`、
`ui_host_drawn_hide|1`）与 `docs/reports/W3-A-isolation-registration.md` §11.4/§11.7。
不得据进程名恢复自绘；如果宿主确实不绘制，默认行为是宿主兼容限制，用户可以选择
`always_self` 作为显式回退。

## 6. 位置与显示

- 位置只来自真实的 TSF 文本范围：`ITfContext::GetSelection` → `ITfRangeACP::SetExtent`
  （UTF-16 code unit ACP）→ `ITfContextView::GetTextExt`，用同步只读编辑会话读取。
- **W3-A 定位来源**：原文归宿主后文档里没有产品组合，因此锚点是
  `[anchor caret - input 长度, anchor caret)` 这一 token 范围，不是组合，也不是合成
  anchor。`anchor caret` 只能等于 `token 起点 + 宿主 token UTF-16 单元数`；token 起点是
  第一个字符交给宿主前记录的投影 caret，随后由真实宿主编辑通知校准。快照 caret 可能
  落后一个通知（8→9），所以展示/选择时读取真实 caret，但只有等于 token 结束位置才接受；
  不会把旧菜单重定位到任意当前光标（即使前面是相同文本）。
- 矩形读取会在同一会话里再次核对 live caret 等于锚点；不相等、无布局、零矩形、不可见、
  读取失败 → 隐藏窗口。**绝不**用鼠标位置或屏幕角落冒充光标位置。live caret 与锚点
  不等时立即失效该菜单；这只是显示与授权同时失效，不是重定位。
- 布局暂不可读时按 pacing 重试，预算 **40 次/宿主 token**，不因候选快照版本变化而重置；
  token 边界、目标绑定/丢失、宿主 `Show(TRUE)` 才重置。预算耗尽仍无矩形 → 关闭该菜单
  （不是留下一个不可见的可键盘选择菜单）。全程不忙等、不在 UI 线程等待布局或引擎。
- 宿主接管绘制（`pbShow=FALSE` 或 `Show(FALSE)`）时默认窗口不创建/隐藏；元素继续回答候选
  查询。唯一例外是用户显式 `candidate_display=always_self`（`pbShow=FALSE` 时仍自绘，
  `Show(FALSE)` 仍强制隐藏）。只有宿主 `Show(TRUE)` 才把绘制权交回 TIP。
- 光标移动、鼠标点击其他位置、选区替换、删除/粘贴等外部编辑都会在宿主编辑通知或下一次
  真实 caret 读取时被观察到；旧版本菜单的选择由版本/caret/范围/文本四重校验拒绝。
  滚动和窗口移动不改变 ACP；新的 `GetTextExt` 只在菜单版本更新时读取，窗口位置可能保持
  到下一次菜单更新或关闭。
- **仍未验证（C 层）**：真实 DPI 变化后的自动重定位、真实多显示器/负坐标、真实
  `GetTextExt` 与宿主绘制协商；受控布局测试不能冒充这些结论（见 §7）。
- 每个引擎页最多显示 8 行（schema `page_size: 8`）。产品当前的最大菜单是
  1 原文 + 最多 7 补全，或 1 原文 + 最多 3 拼写建议，因此一页即末页；页脚显示真实页号。
- 窗口按监视器工作区夹取，底部空间不足时翻到光标上方；DPI 变化重建字体并重新定位；
  小工作区不溢出。`WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`，不使用
  `SetForegroundWindow` / `SetFocus`。

## 7. 已验证与未验证

- 已验证（B 层，受控）：真实 Win32 窗口的绘制、8 行、点击索引、`WM_MOUSEACTIVATE=MA_NOACTIVATE`、
  截图；真实 `ITfCandidateListUIElement` 对象在受控假管理器下的 Begin/Update/End、
  `pbShow` 初值、宿主 `FALSE` 决定、FALSE 后的首次全量 Update、`Show(FALSE)`/`Show(TRUE)`/
  `IsShown`、无选择 `GetSelection=S_FALSE` 与无选择 Finalize、宿主 End、选择/最终化回调、
  版本与 identity 传递；100/150/200% 度量与多显示器/小工作区的纯布局；方向键/Enter/
  Space/Escape 的文档结果、过期版本与外部编辑拒绝；token 锚点纯策略（8→9 接受、异位
  `hel` 拒绝、未知 token 失败关闭）与每 token 40 次布局重试预算；非折叠选区下的旧选择拒绝。
- W3-A 新增 B 层证据：真实控制器/协调器/编辑层的替换故障注入（写前拒绝、读回不一致、
  排队后宿主继续编辑、同文本他处、焦点切换、`SetText` 成功后报错的不确定状态）、
  丢弃后建议流重同步、真实 caret 锚点读取。详见
  `docs/reports/W3-A-isolation-registration.md`。
- W3-B 新增 B 层证据（`product.ui.candidate_window` 与
  `product.ui.candidate_display_can_fail`）：态 A（TRUE 时 TIP 窗口）、态 B 默认不自绘、
  态 B + `always_self` 自绘、`Show(FALSE)` 后不重开、`Show(TRUE)` 恢复、态 C 无
  `ITfUIElementMgr` 自绘且点击携带真实快照 identity、态 A/C 无布局不打开窗口；
  `settings.ini` 的 `candidate_display` 往返与设置窗口读写、下一次引擎启动生效的设计。
  控制条故意断言“态 B 默认自绘”，必须以 14 退出。真实宿主中的 A/B/C 观测仍未执行
  （C 层，待授权）。
- **未验证（C 层）**：修正后的正式包（含 `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` 注册、
  `pbShow` 严格遵从、FALSE 后首次 Update、`S_FALSE`）在真实应用/StrictTsf 前台下的
  宿主绘制行为与点击、真实 `ITfUIElementMgr`、真实 `GetTextExt` 矩形、真实 DPI/多显示器。
  本机没有已授权的隔离环境；直接 sink 变体不创建 TSF UI 管理器。trace 构建曾通过，但
  其 stage8 自绘路径违反 `pbShow` 契约，不能用作正式链路证据。该门停留在 W3-A 的授权
  请求处；如果 Notepad 在修正后仍不绘制，只能记录宿主限制，不能用自绘绕过。

## 8. W2-B-R 审查结论（实现未改变，补充验证）

- **无显式选择时的 `GetSelection`**：W3-A 修正为 `S_FALSE`，`*puIndex` 无效。TSF 的
  UILess 文档明确要求无选择时返回 `S_FALSE`；W2-B-R 当时断言 `S_OK`/索引 0 是错误期望，
  已按契约修正，而不是为了通过测试放宽实现。`Finalize` 在无选择时不会隐式接受原文行。
- **更新标志**：每次 `UpdateUIElement` 都携带完整集合
  （`TF_CLUIE_DOCUMENTMGR | COUNT | SELECTION | STRING | PAGEINDEX | CURRENTPAGE`）。
  这是保守做法：宿主会重读全部数据；首次 Begin 之后不会出现“缺标志导致旧数据”的状态。
  宿主返回 `FALSE` 后立即发送的第一次 `UpdateUIElement` 也携带全量标志。
- **`pbShow=FALSE`**：仍然更新元素数据并回答所有查询，只是不创建/显示自绘窗口；不重复
  弹窗。没有进程名白名单；Notepad 的 `FALSE`-但不绘制行为作为宿主限制记录。
- **End 之后元素仍被合法持有**：`EndUIElement` 返回后管理器仍可查询元素；只读查询继续
  返回最后一页数据。元素引用由 `End()`/`Stop()` 在管理器调用返回后释放。
- **Stop 之后的迟到回调**：桥接对象被置空/inactive；迟到 `SetSelection`/`Finalize` 不
  会到达控制器。`product.ui.candidate_window` 覆盖该路径。
- **模块记账**：候选元素与 UIElement sink 计入 `DllCanUnloadNow`（见引擎生命周期契约
  §11.3），避免 TSF 在 DLL 卸载后释放它们。
- **仍未验证（C 层）**：正式注册 TIP 的真实 `ITfUIElementMgr`、真实前台应用中的窗口
  显示/点击不抢焦点、真实 `GetTextExt`/`ReadTokenRect`、150/200% DPI 与多显示器。
  受控直接 sink 变体不创建 TSF UI 管理器，不能用它替代这些结论。

## 9. W2-C 替换路径的编辑层验证

`windows.tsf_edit.replace_token` 用真实的 RequestEditSession/锁回调覆盖：

- 成功替换只改 caret 前的 token（`abc hel` → `abc hello`），不启动产品组合；
- 同一范围但文本不同（`abc hex`）→ 拒绝，`SetText` 调用数不变；
- 更长 token 的后缀（`abchel`，expected `hel`）→ 拒绝，长 token 不变；
- 空替换 → 计划构建阶段拒绝（`kInvalidSelection`），永不删除原文；
- 非折叠选择 → 拒绝（`kSelectionNotCollapsed`）；
- TSF 为范围写入开/关的组合计数在结束时为 1/1（无残留组合）。

## 10. W3-A 替换故障注入与重同步（B 层）

`product.integration.sink_replacement_faults` 用真实控制器、协调器、真实 TSF 编辑会话与
可注入的 TestTextStore 覆盖：

| 场景 | 宿主真相 | 断言 |
| --- | --- | --- |
| 菜单后 caret 移到同文本另一处 | 文档不变 | 旧版本选择被拒；无 SetText；文档逐单元等于独立参考 |
| 替换排队（异步锁）时宿主继续输入 | 新输入已插入 | 过期计划不覆盖；仅宿主的 SetText；文档等于参考 |
| 菜单展示后 token 被外部改成同长文本 | 新文本在文档中 | 读回不一致拒绝；无产品 SetText |
| 替换排队时焦点切到新上下文 | 两个文档都不变 | 旧计划不写入任一文档；父文档保持原文 |
| `SetText` 已改文本但返回失败 | 文本真的变了 | 不回滚、不重复；`internal_text_losses += 3`；后续宿主输入保留；上下文门关闭；新上下文可正常增强 |

oracle 是测试自己维护的 `ReferenceDoc` 与桩存储真实文本，不读取产品的 dropped/refused
计数器来修剪期望。失败日志会打印计数，但“通过”取决于文档比较。

`product.integration.sink_suggestion_resync` 覆盖丢项后的重同步：单 token 突发使 64 项建议
队列溢出后，`token_abandoned` 保持到 token 边界；期间继续输入不会把后缀当成完整前缀，
也不会开菜单；边界后新 token 正常产生菜单并安全替换。`internal_text_losses` 保持 0。
