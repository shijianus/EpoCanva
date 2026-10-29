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

W2-C 起，选择**不再是引擎命令**，而是一次显式的原文替换
（`ContextCoordinator::SubmitSuggestionReplacement`）。只有以下全部成立才继续：
快照有效、版本一致、快照 identity 与请求一致、当前绑定 identity 与请求一致、
索引在该页内。否则返回 `kCandidateStale`，计入 `candidate_stale_refusals`，不提交任何编辑。

真正的写入由 `CompositionWriter::ReplaceToken` 完成：它读取折叠 caret，取 caret 前
`expected` 长度的范围，逐单元读回并比较；文本不同、范围越界、选择非折叠、前一个字符
仍是 token 字符、替换文本为空都会**拒绝且不调用 SetText**。因此焦点变化、外部编辑、
绑定重建后的旧点击不会删除或覆盖用户已经输入的原文；最坏结果是增强被拒绝，原文保留。

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
- **W2-C 限制**：原文归宿主后文档里没有产品组合，`UpdateCandidateUi` 只在
  `coordinator.composition_active` 时读取组合矩形；建议模式下该值为 false，因此受控窗口
  没有 TSF 文本矩形可锚定。受控测试全部走宿主绘制（`pbShow=FALSE`）路径；真实
  `ITfUIElementMgr` 下自绘窗口的位置仍属 C 层未验证项，不用鼠标位置或屏幕角落冒充。
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
  显示/点击不抢焦点、真实 `GetTextExt`/`ReadCompositionRect`、150/200% DPI 与多显示器。
  受控直接 sink 变体不创建 TSF UI 管理器，不能用它替代这些结论。

## 9. W2-C 替换路径的编辑层验证

`windows.tsf_edit.replace_token` 用真实的 RequestEditSession/锁回调覆盖：

- 成功替换只改 caret 前的 token（`abc hel` → `abc hello`），不启动产品组合；
- 同一范围但文本不同（`abc hex`）→ 拒绝，`SetText` 调用数不变；
- 更长 token 的后缀（`abchel`，expected `hel`）→ 拒绝，长 token 不变；
- 空替换 → 计划构建阶段拒绝（`kInvalidSelection`），永不删除原文；
- 非折叠选择 → 拒绝（`kSelectionNotCollapsed`）；
- TSF 为范围写入开/关的组合计数在结束时为 1/1（无残留组合）。
