# W2 键盘输入与 STA 唤醒契约（W2-C：原文归宿主）

本文件只写 **已实现的产品契约**。它不是任务书，也不替代 `AGENTS.md` 的永久规则。
涉及 librime/TSF 行为的断言以磁盘源码为准（librime `33e7814`，Windows SDK 10.0.26100.0）。

W2-C 的根规则只有一条：**普通原文永远由宿主承载；引擎只产生可取消的增强建议。**
产品不再为正文维护队列、容量、raw 捕获或重放路径，因此内部队列、容量、引擎延迟和恢复
过程都不再可能成为丢字原因。历史 W2-A-R/W2-B 的 raw 路径已从产品控制器删除
（`key_admission.{h,cc}` 作为独立库仍存在并受 `product.unit.key_admission` 覆盖，
但产品不再引用）；其数值归属见 §6。

## 1. 生命周期

`EpoCanvaTextService::ActivateInternal`（`platforms/windows/tsf/text_service.cc`）的顺序是：

1. 构造 `ProductTextController` 并以**字面（literal）模式**启动唤醒通道；
2. `ProductEngineBootstrap::RequestStart()` 在后台线程执行部署事务 `Prepare`、
   `EngineHostBroker::Acquire`、`Complete` 与租约锁交接；**Activate 不等待它**；
3. `ITfKeystrokeMgr::AdviseKeyEventSink`；
4. 提交新状态、释放旧控制器/旧线程管理器。

引擎启动完成后，后台线程唤醒 STA；STA 在自己的 `PumpNow` 中采纳租约、构造
`ContextObserver`（**StrictTsf**，`TextOwnershipPolicy::kHostOwned`）与协调器。
启动期间的每个键保持宿主默认效果（原文路径），不排队、不组合、不写文档。

任一步失败都不修改当前活动状态。`Deactivate` 先 `UnadviseKeyEventSink`，再停止控制器；
它**不 join** 仍在进行的引擎启动，也不在 STA 上释放可能阻塞的租约（租约释放被移到
持有模块引用的后台线程）。`DllMain` 仍只保存 HINSTANCE。

启动等待（命名互斥量/共享锁最长 60 s，以及 `Acquire` 内的部署）只在后台启动线程发生；
`Activate`、`OnKeyDown`、`Deactivate` 与卸载检查都不承受它。失败由
`EngineBootstrapState::kFailed` 与 `ControllerDiagnostics::engine_state/failure` 报告，
下一次激活可重试；`DllCanUnloadNow` 在启动线程退出前保持 `S_FALSE`。

产品导出只有 `DllCanUnloadNow` 与 `DllGetClassObject`（`epocanva_tsf.def`）。
直接 sink 测试变体（`epocanva_tsf_direct_sink_test.dll`）额外导出受控测试符号
（`EpoCanvaTest*`），只用于 `tests/windows/tsf_product`，不进入产品 DLL，也不进入暂存包。

## 2. 按键语义（W2-C）

| 回调 | 行为 |
| --- | --- |
| `OnTestKeyDown` | 预测。规则与 `OnKeyDown` 完全一致；**只有显式菜单交互**可能返回 TRUE。纯文本、Space、Enter（无显式选择）、BackSpace、Escape（无菜单）、快捷键恒 FALSE。不调用引擎、不启动许可查询、不请求编辑。 |
| `OnKeyDown` | 同上。返回 TRUE 的键从不产生原始文本：方向键移动菜单选择，Enter 接受显式选择，Escape 关闭菜单。其它一律 `pfEaten=FALSE`，宿主按自己的默认路径插入/删除/换行。 |
| `OnTestKeyUp` / `OnKeyUp` | 恒 `FALSE`；不翻译、不派发。 |
| `OnPreservedKey` | 恒 `FALSE`；本阶段不注册保留键。 |

`DecideKey`（`platforms/windows/input/key_decision.cc`）仍是唯一一套字符规则；控制器用它
区分“可进入引擎建议流的 token 字符”和“边界/快捷键/隐私拒绝”，但**不再用它决定是否吃键**。
Ctrl/Alt/Win（含 AltGr 的 Ctrl+Alt）直通；数字与技术标记
（`@ . _ : / \ ? & = # % + -`）属于 token 字符，不是边界。

隐私：`Denied` / `Unknown` / 输入被禁用时，控制器清空建议流、丢弃 token 长度，
**不向引擎发送任何文本**，也不读取字段内容。字段内容本身从未由产品持有。

## 3. 原文归宿主、建议可丢弃

原文的承载者是宿主文档。产品在普通输入期间：

- 不排队、不缓存、不捕获、不写盘、不重放任何用户文本；
- 不启动文档编辑会话（因此没有文档写授权延迟影响原文的问题）；
- 不把 `retained + dropped == typed` 当作正确性条件，因为不存在产品侧的 retained/dropped。

引擎建议是唯一的异步工作：

- 每个可进入引擎的 token 键被复制成 `SuggestionKey`（事件 + 翻译文本 + decision +
  context/generation），进入**有界 64 项**的 `suggestion_queue_`；
- 队列满、引擎忙、绑定未就绪、许可 Unknown/Denied、流已过期时，建议被**明确丢弃**
  （`suggestion_dropped`），菜单关闭；原文不受影响；
- 同一时刻最多一个建议键在途；顺序提交；不因为建议延迟而阻挡下一个宿主键；
- `ContextCoordinator::SetSuggestionMode(true)` 保证引擎观察**只**更新候选快照，
  绝不产生文档编辑（`ResolveEngineResult` 在建议模式下直接 `kNoEdit` 结束）；
- token 结束时（Space/Return/标点/编辑键/导航），控制器清空建议队列，并在没有在途工作时
  向引擎发送一个**合成的 Escape**用于取消引擎内部组合。该 Escape 只存在于建议流，
  不进入文档；不能提交时被丢弃，不会重试到超时。

跨应用/外部编辑：观察者把宿主的插入/删除（`TextOwnershipPolicy::kHostOwned`）视为
**预期的原文流**，只更新 caret 镜像，不改变 identity/generation，因此绑定和建议流可以继续。
一个没有被任何直通键解释的编辑（粘贴、自动更正、另一客户端）会结束当前建议流
（`host_edits_unexpected`），但原文仍然属于宿主，产品不读取、不恢复、不覆盖。

焦点/文档切换：建议队列被丢弃，在途结果丢弃，候选菜单关闭；**没有**“已接受原文”需要
取消或回滚的账目，因为原文从一开始就在自己的文档里。旧 generation 的建议不会写入新文档：
replacement 提交仍校验快照版本、快照 identity、当前 identity 与目标。

## 3.1 候选选择（显式替换）

候选菜单的读取与选择路径见 `docs/windows/candidate-ui-contract.md`。输入侧：

- 菜单只在以下全部成立时可选：快照有效且版本一致、没有建议在途/排队、引擎 token 的
  UTF-16 长度等于宿主当前 token 长度、版本未被 Escape 取消；
- Enter（有显式选择）/鼠标 Finalize 走 `SubmitSuggestionReplacement`：
  无引擎命令，校验快照版本 + 快照 context/generation + 当前绑定 identity + 页内索引；
- 真正的写入由 `CompositionWriter::ReplaceToken` 在授予的编辑会话里完成：
  读取折叠 caret，取 caret 前 `expected` 长度的范围，**逐单元读回并比较**；
  不相等、范围越界、选择非折叠、前一个字符仍是 token 字符、替换文本为空 → **拒绝**，
  并且 **不调用 SetText**；原文不会被删除或覆盖；
- 成功替换后，引擎流被重置，下一个 token 重新开始；替换本身是唯一的文档写入路径。

受控测试观测：`ITfRange::SetText` 在该环境下会让 TSF 为这次范围写入开/关一个组合
（store 计数 start/end 各 1，结束时无活动组合）。这只由编辑层验证，不是产品持有的组合。

## 4. 固定 STA 唤醒

`StaResultWaker`（`platforms/windows/tsf_product/sta_result_waker.h`）：

- 引擎 worker 只 `SetEvent(EngineExecutor::ResultReadyEvent())`，不接触 COM；
- 一个 watcher 线程只 `WaitForMultipleObjects` + `PostMessageW`，不接触 COM；
- 消息只有 `WM_APP+0x51`，`wParam` 是注册 generation，`lParam=0`，无文本、无借用对象；
- STA 的窗口过程核对 HWND 注册与 generation，再调用 `OnStaWake` → `PumpNow`；
- 窗口类用**产品 DLL 自身**的 `ModuleInstance()` 注册，引用计数，最后一个 waker 停止时
  `UnregisterClassW`；绝不借用宿主 EXE 的 HINSTANCE（否则 DLL 卸载后类指向已卸载代码）；
- `Stop` 先停 watcher、再注销 HWND、最后 `DestroyWindow`；HWND 复用或迟到消息到不了旧对象；
- `PostMessage` 失败会计数并以 10 ms 间隔重试；结果始终留在执行器里，唤醒失败不等于已应用；
- TSF 异步许可/编辑完成没有可等待的 Win32 事件，watcher 在有未完成工作时以 10 ms 间隔唤醒，
  空闲时无限等待。每次 `Pump` 都是幂等的，重复唤醒不会重复消费。

## 5. 部署一致性

`EngineStartTransaction`（`platforms/windows/product/product_engine_config.cc:283`）：

- 快照 = settings token、schema/default/custom、product lexicon、rime.dll 的 SHA-256；
- 成功启动后写 `<user>/engine/deployed-state`，**只有写成功后才**按 token 删除 `deploy-required`；
- 启动失败/产物缺失：保留 marker，下次重试；
- 同一进程：`EngineHostBroker` 成功后清掉配置中的一次性 `deployment`，后续租约不重复部署；
- 跨进程：命名互斥量序列化启动/部署；`engine.lock` 的字节范围锁区分
  **独占部署**（先删除派生的 `engine/build` 强制重编译）与 **共享运行**（每个活动引擎持有）；
- `ActivateInternal` 在 `Complete` 之后调用 `EngineStartTransaction::TakeLeaseLock()`，
  共享锁句柄由激活状态持有到 `Deactivate`（或异常析构）；`Deactivate` 先释放引擎租约
  （停止/加入执行器）再关闭锁句柄；
- 有其它进程运行时，独占锁失败，本次部署明确**降级为共享执行**：新宿主仍可激活/输入旧配置，
  设置 marker 保留，推迟到下一个真正的引擎启动；不会无限等待，也不会把共享锁升级为独占；
- 运行中的引擎不会被重新部署；设置修改在下一次引擎启动/宿主重启生效，界面文案已按此声明。

## 6. 历史归属：W2-B-R 的 1202 / 1024 / 177（已由 W2-C 取代）

以下是对旧 raw 路径（HEAD `8474e08` 的 `5a5edd2` 实现）的**更正后**归属。
旧报告 §5 写的“已应用 1024、明确丢弃 177、retained + dropped == typed”并不完整：
它把 1 个已应用的引擎键排除在“已应用”之外，并把内部容量丢失写成了“明确丢弃”。

固定输入 `product.integration.sink_capacity_continue`：`ab` + 1200 × `x`，
1202 个 key-down 事件，每个键恰好 1 个 UTF-16 code unit（ASCII，所以两种单位数值相同，
但含义不同；不得直接相加）。真实异步文档锁被持有，首个引擎键在途。

| 归属 | 实际值 | 依据 |
| --- | --- | --- |
| 输入事件（key-down） | 1202 | `SendTextAdaptive` 每单元一个键 |
| 文本 UTF-16 单元 | 1202 | 全部 ASCII，1 键 = 1 单元 |
| 已接受（`pfEaten=TRUE`） | 1202 / 1202 | 旧 `HandleRawKey` 对可捕获文本无条件返回 `true` |
| 已由宿主默认处理 | **0** | 所有键都被产品吃掉，宿主没有机会插入 |
| 已应用 | **1025** = 1（首个引擎键 `a`）+ 1024（raw 块） | `raw_applied_units == 1024`；文档长度为 1025 |
| 仍待应用（结算后） | 0 | 队列/在途归零；块只写一次 |
| 用户明确取消 | 0 | 没有 Escape/取消 |
| 内部丢失 | **177** | 键 1026–1202（最后 177 个 `x`） |

177 的触发链（逐行可核对）：

1. 第 1 个键 `a` 被提交给引擎，其编辑会话因文档锁未授予而在途；
2. 第 25 个键触发高水位（`QueueDepth() >= kHighWater = 24`），
   `EnterRawMode()` 撤出队列尾部（键 2–24，23 单元）；
3. 后续文本键进入 `CaptureRawText`，填满 `RawTextRun::kDefaultCapacityUtf16 = 1024`
   （键 2–1025，恰好 1024 单元）；
4. 第 1026 个键的 `Append` 返回 `kNeedsFlush`；`FlushRaw()` 因
   `HasPendingEngineInput()`（第 1 个键仍未解决）立即返回；重试 `Append` 仍为
   `kNeedsFlush`，于是 `RecordRawDropped(1, 1)`、`raw_unrecoverable_++`；
5. `HandleRawKey` 忽略 `CaptureRawText` 的 false，仍然 `return true`，所以这些键
   `pfEaten=TRUE` 但文本没有进入任何文档；键 1026–1202 共 177 个；
6. 锁释放后，第 1 个键的编辑与 1024 单元 raw 块依次应用，文档为 1025 单元。

因此 177 不是焦点变化、退役或用户取消，而是**容量不足 + 在途键阻塞 flush 的内部丢失**，
且发生在已经对宿主承诺 `pfEaten=TRUE` 之后。旧用例的参考期望由产品自身状态构造
（`retained = burst.substr(0, retained.size())`，再用 `retained.size() + dropped_keys ==
typed_units` 判通过），所以把丢失当成了合法期望；这是测试 oracle 的缺陷，不是输入保真。

W2-C 删除整条 raw 路径后，同样的输入得到：1202 个键全部 `pfEaten=FALSE`、
宿主插入 1202 单元、产品应用 0 单元、内部丢失 0、仍待应用 0，最终文档等于完整参考。
`docs/reports/W2-C-input-fidelity.md` 记录实测。

## 7. 计数单位与验收口径（W2-C）

- 长度与偏移一律标注 **UTF-16 code unit**；键事件数与文本单元数是两个单位，不能相加。
- 独立参考模型只由用户的动作生成（宿主插入、宿主 Backspace、显式候选替换），
  从不读取产品的 dropped/failed 状态来修剪期望。
- A 类（无取消、无文档切换）：最终文档必须等于完整参考；`internal_text_losses == 0`；
  队列/在途为 0。
- B 类（BackSpace/Escape/Enter）：按本文件 §2/§3.1 的公开契约计算参考；
  内部溢出不得归入这些操作。
- C 类（焦点/文档切换）：逐文档验证；旧候选不得写入新文档；原文不因 generation 改变
  被记作用户取消（W2-C 根本没有原文取消账目）。
- `pfEaten=TRUE` 的文本必须有实际归属：控制器只在菜单交互上返回 TRUE，测试的逐键账本
  （`InputRoute::product_suppressed_units`）必须为 0。

### 7.1 已知限制（诚实报告）

- 建议模式下文档里没有产品组合，`UpdateCandidateUi` 不读取 `GetTextExt` 矩形；真实
  TSF UIElement 管理器下的窗口位置/显示尚未在 C 层验证（受控测试用宿主绘制路径）。
- 一次替换或外部编辑之后，当前 token 的剩余字符可能不再产生菜单（引擎流被重置，
  而产品不保存 token 文本用于重放）；下一个 token 正常。建议可延迟/丢弃，原文不受影响。
- 直通键的 host-edit 记账是“一键一编辑”的信用模型；应用把多次插入合并为一次编辑、
  或 BackSpace 在文档开头无效果时，信用可能剩余，使下一次外部编辑被当作预期编辑。
  这只会影响建议流，不改变原文；列为残余风险。
- UTF-16 代理项对的原子追加/回退由 `product.unit.key_admission` 与编辑层测试覆盖；
  B 层键盘路径没有合法的 astral 输入，未用合成事件冒充。
