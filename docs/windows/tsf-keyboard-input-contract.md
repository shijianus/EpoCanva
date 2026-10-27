# W2-A 键盘输入与 STA 唤醒契约

本文件只写 **W2-A 已实现的产品契约**。它不是任务书，也不替代 `AGENTS.md` 的永久规则。
涉及 librime/TSF 行为的断言以磁盘源码为准（librime `33e7814`，Windows SDK 10.0.26100.0）。

## 1. 生命周期

`EpoCanvaTextService::ActivateInternal`（`platforms/windows/tsf/text_service.cc:209`）的顺序是：

1. 产品部署事务 `Prepare`：读取设置/部署快照、取得跨进程启动锁、安装引擎配置；
2. `EngineHostBroker::Acquire`：启动或共享进程内唯一引擎；
3. 产品事务 `Complete`：校验编译产物、记录部署快照、按 token 退役标记；
4. 构造 `ProductTextController` 并订阅 `ContextObserver`（**StrictTsf**）；
5. `ITfKeystrokeMgr::AdviseKeyEventSink`；
6. 提交新状态、释放旧租约/旧线程管理器。

任一步失败都不修改当前活动状态；新租约、新控制器、新锁会按相反顺序释放。
`Deactivate`（`:140`）先 `UnadviseKeyEventSink`，再 `ProductTextController::Stop`，
然后释放线程管理器与引擎租约。`DllMain` 仍只保存 HINSTANCE。

产品导出只有 `DllCanUnloadNow` 与 `DllGetClassObject`（`epocanva_tsf.def`）。
直接 sink 测试变体多一个编译宏，不导出任何测试符号，也不进入暂存包。

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
2. 降级期间的文本键、Space、Enter、Escape、BackSpace 继续被吃下并折叠进这段文本：
   Space 追加 U+0020 并结束 token；Enter/Escape 结束 token 但不取消已捕获文本（记为
   `raw_ignored_edits`）；BackSpace 只回退捕获文本中的最后一个 code unit，捕获为空时记为
   被忽略的编辑。快捷键与隐私拒绝永远不进入捕获。
3. 协调器空闲时用新的 `SubmitRawCommit` 把这段文本一次提交到文档：若写者仍拥有组合，
   plan 用“当前显示文本 + 捕获后缀”替换并结束组合；否则在跟踪的 caret 处插入。
   **这次提交不调用引擎**，因此没有补全/拼写建议——原文优先。
4. 提交成功后引擎客户端状态已过期，控制器使绑定退役并重建；重建完成后引擎路径恢复。
   降级不会把后续字符散给宿主，也不会在超时后自动重放。
5. 捕获上限 `kRawCapacityUtf16 = 1024`。超过上限的键仍被吃下以保住已有顺序，但会记入
   `raw_dropped_keys` / `failed_raw_capacity`（`last_failure=raw-capacity`），
   **不声称该键已应用**。提交失败同样走 `raw-commit-failed` 计数并以明确失败结束该段。
6. 隐私/许可在降级途中变成 Denied/Unknown 时，已捕获文本无法再写入：整段记为
   `kPermissionRefused` 失败，宿主获得之后的按键。这是不可恢复故障的明确报告，不是静默丢弃。

**首 token 规则**（`HostTokenOwnership`，`platforms/windows/tsf_product/key_admission.h`）：
一旦某个 token 的第一个字符交给宿主（许可 Unknown/Denied、引擎未就绪、队列满且无待应用
输入等），该 token 余下字符一律直通，直到空格、非 token 标点、编辑键或焦点真正移走。
`@ . _ : / \ ? & = # % + -` 与字母数字一样是 token 字符，不是边界；快捷键不结束 token。
许可在 token 中途变为 Allowed 也不会让引擎接管该 token。Unknown/Denied 从不向引擎发送输入。

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
