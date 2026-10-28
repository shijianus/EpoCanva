# 候选窗口与 TSF UIElement 契约（W2-B）

本文件只写 **W2-B 已实现**的候选交互。它不是任务书，也不替代 `AGENTS.md` 的永久规则。
TSF 接口以磁盘上的 `msctf.h`（Windows SDK 10.0.26100.0）为准。

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

`ContextCoordinator::SubmitCandidateSelection` 只有在以下全部成立时才调用引擎：
快照有效、版本一致、快照 identity 与请求一致、当前绑定 identity 与请求一致、组合仍活动、
索引在该页内。否则返回 `kCandidateStale`，计入 `candidate_stale_refusals`，不派发任何
引擎命令。因此焦点变化、外部编辑、raw 模式、绑定重建后的旧点击不会提交到新文档。

## 4. 键盘与鼠标

| 输入 | 行为 |
| --- | --- |
| ↑ / ↓（无修饰） | 菜单打开时被吃掉，循环移动显式选择；初始从“无选择”进入原文/末项 |
| Enter | 有显式选择 → 走第 3 节的校验选择；无显式选择 → 保持原引擎 Enter 契约（提交原文/引擎高亮项） |
| Space | 不拦截，始终按原文提交契约处理（空格处理器提交原文 + U+0020） |
| Escape | 关闭菜单/清空选择，并走引擎 Escape 取消组合 |
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
- 无布局、零矩形、不可见、无焦点、读取失败 → 隐藏窗口；**绝不**用鼠标位置或屏幕角落冒充
  光标位置。
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
- **未验证（C 层）**：正式注册的 TIP 在真实应用/StrictTsf 前台下的候选窗口显示与点击。
  本机没有已授权的隔离环境；直接 sink 变体不创建 TSF UI 管理器。该门留给 W3。
