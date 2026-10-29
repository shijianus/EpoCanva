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
- **该菜单展示时从真实 TSF 选区读到的折叠 caret ACP**（UTF-16 code-unit 偏移）

W3-A 起，caret 是菜单身份的一部分：引擎观察可能早于宿主最后一次编辑通知，因此快照里的
caret 镜像可能落后一个通知。控制器在菜单版本首次展示时用只读 `GetSelection` 读取真实
caret，把它记为这一版本的锚点（不读文本），并把差异回填给协调器镜像。选择请求必须
携带这个锚点；协调器只接受 `caret >= input 长度` 的位置，真正的等价校验在写入会话内。

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

- 通过 `ITfUIElementMgr::BeginUIElement` 注册每个候选菜单；`UpdateUIElement` 更新；
  `EndUIElement` 结束。元素实现 `ITfCandidateListUIElementBehavior`，提供
  `GetCount/GetString/GetSelection/GetPageIndex/GetCurrentPage` 与
  `SetSelection/Finalize/Abort`；`SetSelection/Finalize` 只回调控制器。
- `BeginUIElement` 返回的 `pbShow` 是宿主的决定：为 `FALSE` 时**不创建**自绘窗口，
  只继续回答候选列表查询；宿主接管绘制时不重复弹窗。
- `ITfUIElementSink` 通过 `ITfUIElementMgr` 的 `ITfSource` 订阅；宿主发起的 End 会隐藏
  窗口并使后续 End 幂等。`EndUIElement` 回调期间不释放元素引用（元素在管理器调用返回后
  才释放），避免 TSF 在回调栈上持有已释放对象。
- 停止/卸载：`CandidateUi::Stop` 先 End、再注销 sink、释放管理器与窗口；桥接对象在停止时
  置空，迟到回调只能看到 inactive。

## 6. 位置与显示

- 位置只来自真实的 TSF 文本范围：`ITfContext::GetSelection` → `ITfRangeACP::SetExtent`
  （UTF-16 code unit ACP）→ `ITfContextView::GetTextExt`，用同步只读编辑会话读取。
- **W3-A 定位来源**：原文归宿主后文档里没有产品组合，因此锚点是
  `[live caret - input 长度, live caret)` 这一 token 范围，不是组合，也不是合成 anchor。
  控制器在菜单版本首次展示时读取真实 caret；读到的 caret 与快照镜像不同时，用真实值
  重同步（`ObserveRealCaret`），不把落后的镜像当成位置。
- 矩形读取会在同一会话里再次核对 live caret 等于锚点；不相等、无布局、零矩形、不可见、
  读取失败 → 隐藏窗口。**绝不**用鼠标位置或屏幕角落冒充光标位置。
- 宿主接管绘制（`pbShow=FALSE`）时窗口不创建；元素继续回答候选查询。
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
  宿主绘制抑制、宿主 End、选择/最终化回调、版本与 identity 传递；100/150/200% 度量与
  多显示器/小工作区的纯布局；方向键/Enter/Space/Escape 的文档结果、过期版本与外部编辑拒绝。
- W3-A 新增 B 层证据：真实控制器/协调器/编辑层的替换故障注入（写前拒绝、读回不一致、
  排队后宿主继续编辑、同文本他处、焦点切换、`SetText` 成功后报错的不确定状态）、
  丢弃后建议流重同步、真实 caret 锚点读取。详见
  `docs/reports/W3-A-isolation-registration.md`。
- **未验证（C 层）**：正式注册的 TIP 在真实应用/StrictTsf 前台下的候选窗口显示与点击、
  真实 `ITfUIElementMgr`、真实 `GetTextExt` 矩形、真实 DPI/多显示器。本机没有已授权的
  隔离环境；直接 sink 变体不创建 TSF UI 管理器。该门停留在 W3-A 的授权请求处。

## 8. W2-B-R 审查结论（实现未改变，补充验证）

- **无显式选择时的 `GetSelection`**：返回 0（原文行）。这与“无选择时 Enter 提交原文”的
  契约一致，宿主不会把补全/拼写建议画成预选项。
- **更新标志**：每次 `UpdateUIElement` 都携带完整集合
  （`TF_CLUIE_DOCUMENTMGR | COUNT | SELECTION | STRING | PAGEINDEX | CURRENTPAGE`）。
  这是保守做法：宿主会重读全部数据；首次 Begin 之后不会出现“缺标志导致旧数据”的状态。
- **`pbShow=FALSE`**：仍然更新元素数据并回答所有查询，只是不创建自绘窗口；不重复弹窗。
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
