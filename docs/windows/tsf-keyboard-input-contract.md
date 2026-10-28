# W2-A/W2-B 键盘输入与 STA 唤醒契约

本文件只写 **已实现的产品契约**（W2-A，W2-B 增补）。它不是任务书，也不替代 `AGENTS.md` 的永久规则。
涉及 librime/TSF 行为的断言以磁盘源码为准（librime `33e7814`，Windows SDK 10.0.26100.0）。

## 1. 生命周期

`EpoCanvaTextService::ActivateInternal`（`platforms/windows/tsf/text_service.cc`）的顺序是：

1. 构造 `ProductTextController` 并以**字面（literal）模式**启动唤醒通道；
2. `ProductEngineBootstrap::RequestStart()` 在后台线程执行部署事务 `Prepare`、
   `EngineHostBroker::Acquire`、`Complete` 与租约锁交接；**Activate 不等待它**；
3. `ITfKeystrokeMgr::AdviseKeyEventSink`；
4. 提交新状态、释放旧控制器/旧线程管理器。

引擎启动完成后，后台线程唤醒 STA；STA 在自己的 `PumpNow` 中采纳租约、构造
`ContextObserver`（**StrictTsf**）与协调器。启动期间的每个键保持宿主默认效果（原文路径），
不排队、不组合、不写文档；`HostTokenOwnership` 防止启动完成时接管半个 token。

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

## 2. 按键语义

| 回调 | 行为 |
| --- | --- |
| `OnTestKeyDown` | 预测。抓取线程键盘状态、`ToUnicodeEx`（不改状态）、`ContextCoordinator::Evaluate`、队列容量检查。**不调用引擎、不启动许可查询、不请求编辑。** |
| `OnKeyDown` | 只有真正接受（入队或提交成功）才置 `pfEaten=TRUE`。快捷键、非引擎候选、无可靠上下文一律 `FALSE`。 |
| `OnTestKeyUp` / `OnKeyUp` | 恒 `FALSE`；不翻译、不派发。 |
| `OnPreservedKey` | 恒 `FALSE`；本阶段不注册保留键。 |

`DecideKey` 的规则是唯一一套字符规则（`platforms/windows/input/key_decision.cc`），
控制器不复制第二套。Ctrl/Alt/Win（含 AltGr 的 Ctrl+Alt）走宿主；数字与技术标记按文本
候选进入引擎，以便 token 层整体保护 URL/邮箱/路径/标识符。

## 3. 有界准入与每键状态

`KeyAdmissionQueue`（`platforms/windows/tsf_product/key_admission.h`）容量 64；无组合时最多
使用 48，剩余 16 个槽位保留给已经进入组合的输入。

每个键的状态：`kNotAccepted`（宿主保留）、`kAccepted`（已吃下、排队或在途）、
`kEngineCompleted`（引擎已消费）、`kEditCompleted`（编辑或“文档已一致”已确认）、
`kFailed`（带 `KeyFailure` 的明确失败）。同一时刻最多一个在途项；队列 FIFO 顺序不被越过。

Focus/generation 变化时：排队项全部记为 `kFocusChanged` 失败；在途项立即标失败但保留到
协调器回到 `Idle` 才释放，下一键不可能越过它。永远不会把旧 generation 的键投递到新文档。

### 压力降级（W2-A-R）

只增大容量不是解决。控制器在**高水位**（`KeyAdmissionQueue::kHighWater = 24`）切换策略：

1. 队列中的**未提交**尾部按 FIFO 撤出（`WithdrawQueued`），逐键折算成一段 UTF-16 原样文本；
   在途的单个引擎键保持不动。被撤出的键记为 `deferred_raw`，**不是失败**。
2. 降级期间的文本键、Space、Enter、Escape、BackSpace 的语义是明确的：
   - Space 追加 U+0020 并结束该段（随后按原文提交）；
   - Enter 结束该段并提交已捕获后缀，不插入换行（与引擎路径提交组合一致）；
   - Escape **取消尚未提交的后缀**：清空捕获并记为 `raw_cancelled`，不把它写成迟到文本；
     已经交给协调器的块不会被撤回，它会按提交时的一次写入生效。取消后该段在块解决时结束；
   - BackSpace 只回退尚未提交后缀中的**一个 code point**（代理项对整体回退）；后缀为空时
     记为被忽略的编辑并被吃掉。它**从不**删除已显示组合、已提交文本或宿主既有文本。
   快捷键与隐私拒绝永远不进入捕获。
3. 协调器空闲时用新的 `SubmitRawCommit` 把这段文本一次提交到文档：若写者仍拥有组合，
   plan 用“当前显示文本 + 捕获后缀”替换并结束组合；否则在跟踪的 caret 处插入。
   **这次提交不调用引擎**，因此没有补全/拼写建议——原文优先。
4. 提交成功后引擎客户端状态已过期，控制器使绑定退役并重建；重建完成后引擎路径恢复。
   降级不会把后续字符散给宿主，也不会在超时后自动重放。
5. 捕获按块进行，每块上限 `kRawTextRun::kDefaultCapacityUtf16 = 1024`。块满时先把整块
   交给协调器，再让下一个键开启新块；追加是原子的，**不会截断代理项对**。只有当当前块
   无法交出（前一引擎键或块仍未解决）而新键又放不下时，后续键才继续被吃下并记入
   `raw_dropped_keys` / `failed_raw_capacity`（`last_failure=raw-capacity`），
   **不声称该键已应用**；已保留的块按 FIFO 应用，不重放、不重复。提交失败同样走
   `raw-commit-failed` 计数并以明确失败结束该段。
6. 隐私/许可在降级途中变成 Denied/Unknown 时，已捕获文本无法再写入：整段记为
   `kPermissionRefused` 失败，宿主获得之后的按键。这是不可恢复故障的明确报告，不是静默丢弃。

**首 token 规则**（`HostTokenOwnership`，`platforms/windows/tsf_product/key_admission.h`）：
一旦某个 token 的第一个字符交给宿主（许可 Unknown/Denied、引擎未就绪、队列满且无待应用
输入等），该 token 余下字符一律直通，直到空格、非 token 标点、编辑键或焦点真正移走。
`@ . _ : / \ ? & = # % + -` 与字母数字一样是 token 字符，不是边界；快捷键不结束 token。
许可在 token 中途变为 Allowed 也不会让引擎接管该 token。Unknown/Denied 从不向引擎发送输入。

## 3.1 候选选择

候选菜单的读取与选择路径见 `docs/windows/candidate-ui-contract.md`。输入侧只做三件事：
方向键（无修饰）在菜单打开时由控制器吃掉并移动显式选择；Enter 在有显式选择时走
`SubmitCandidateSelection`（快照版本 + 快照 context/generation + 页内索引），没有显式选择时
保持原引擎 Enter 契约；Escape 关闭菜单并走引擎 Escape 取消组合。Space **不**被候选层拦截，
始终按原文提交契约处理。初始没有显式选择，建议不会被自动接受。

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
- W2-A-R：`ActivateInternal` 在 `Complete` 之后调用 `EngineStartTransaction::TakeLeaseLock()`，
  共享锁句柄由激活状态持有到 `Deactivate`（或异常析构）；没有这一步时共享锁随事务结束而释放，
  另一个进程的部署会取得独占锁并删除仍在使用的编译产物。`Deactivate` 先释放引擎租约
  （停止/加入执行器）再关闭锁句柄；
- 有其它进程运行时，独占锁失败，本次部署明确**降级为共享执行**：新宿主仍可激活/输入旧配置，
  设置 marker 保留，推迟到下一个真正的引擎启动；不会无限等待，也不会把共享锁升级为独占；
- 运行中的引擎不会被重新部署；设置修改在下一次引擎启动/宿主重启生效，界面文案已按此声明。

## 6. W2-B-R 增补：同会话容量恢复、降级控制键与事件归属（已实现）

### 6.1 同会话恢复回归

`product.integration.sink_capacity_continue` 在**同一激活、同一上下文**中完成：文档锁
（真实异步锁，而非“不泵消息”）被持有 → 首个引擎键在途 → 发送 `ab` + 1200 个 `x` →
跨越高水位并填满 1024 单元原文块 → 越界键被吃掉并记为 `raw_dropped_keys` → 释放锁并
应用整块 → **立刻**继续输入 `after`（无人工稳定窗口）→ Space 结束并提交 → 引擎路径
在同一会话内恢复 → 输入 `hel`、方向键选择补全、Enter 提交 → 停用并清理。

事件归属（固定输入，1202 个 UTF-16 单元）：

| 归属 | 观测 |
| --- | --- |
| 已接受 | 全部 1202 个键 `pfEaten=TRUE`；`eaten == key_downs` |
| 已应用 | `raw_applied_units == 1024`；文档 = 键入前缀（含首引擎键），长度 1024/1025 |
| 明确丢弃 | `raw_dropped_keys == 177`；`retained + dropped == typed` |
| 取消 | 0 |
| 待恢复 | 队列/在途均为 0；之后引擎菜单与选择路径恢复 |
| 重复/迟到 | 300 ms 后文档不变；容量块只应用一次 |

该用例结束断言模块已卸载；同会话恢复不再依赖重新激活。

### 6.2 降级期控制键端到端

`product.integration.sink_degraded_keys` 通过真实控制器、协调器与 TSF 编辑会话：

- **BackSpace**：真实 backlog 下的未提交后缀回退一个 code point；文档与预期完全相等，
  不是“忽略”。
- **Enter**：结束降级段并提交后缀，不插入换行；此前已提交块保留。
- **Escape**：只取消未提交后缀；已交给协调器的首块仍按提交时一次生效；取消量被计数，
  300 ms 后无迟到写入。
- **空后缀 BackSpace**：被吃掉并记为 `raw_ignored_edits`；不删除已提交/在途文本。
- **焦点切换**：未提交 run 整体记为明确失败（`raw_failed_units`），既不写入旧文档也
  不重放到新文档；新上下文只接受自己的 token。
- **停用竞争**：`sink_unload_contract` 在队列中有编辑会话时停用；陈旧会话不写文档，TSF
  释放回调后模块才可卸载（见 `tsf-engine-lifetime-contract.md` §11）。

### 6.3 测试就绪门

`OnTestKeyDown` 在降级运行中也会为“将被原文捕获”的键返回 TRUE；因此 `ReadyToType`
不能作为“引擎路径已恢复”的判据。测试新增 `WaitForEnginePath`：要求 raw 运行关闭、
`raw_unapplied_units == 0`、队列/在途为 0、协调器空闲且引擎就绪，再做一次预测。

### 6.4 未改变的正常模式语义

Enter/Escape/BackSpace 在正常引擎路径、候选菜单下的行为未改动；降级路径的折衷仍与
§3 一致。未新增数字候选快捷键、自动空格或自动建议接受。
