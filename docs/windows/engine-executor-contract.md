# Windows 引擎执行器契约（P3.5：进程内单线程执行器）

本文档描述 `platforms/windows/engine_executor/` **当前实际是什么**：一个专用工作线程、
一个工作线程内的 `RimeRuntime`、一张工作线程内的 Session/Controller 表、一个有界命令
入口和一个有界、可查询的结果存储。本轮**不**接入 TSF sink，**不**注册输入法，**不**修改
宿主文本，**不**实现候选 UI，**不**把执行器加入 `epocanva_tsf.dll` 的激活路径。

本轮的目标是让多个调用线程可以用**值类型命令**操作 P3.4 的线程亲和引擎，而不是把
`RimeRuntime`、`RimeSession` 或 `InputController` 交给调用方。

## 1. 结论先行

- 每个 `EngineExecutor` 实例拥有一个工作线程；`RimeRuntime` 的 `Start`、`CreateSession`、
  `process_key`、`select_candidate`、状态读取、Session 关闭、`Shutdown` 和 DLL 释放全部
  发生在这个工作线程上。
- 调用方只能提交六种命令，读取自己请求的结果。公共头文件中没有任何返回
  `RimeRuntime`/`RimeSession`/`InputController`、DLL 句柄或借用引擎指针的方法。
- 命令进入一个全局 FIFO；同一个客户端的事件顺序因此稳定。本轮不做并行引擎执行。
- 容量是**已接受但尚未取走**的请求总数：排队中 + 执行中 + 已完成未取走。满时
  `TrySubmit` 返回 `kQueueFull`，不阻塞、不丢旧请求、不覆盖旧结果。取走结果后额度恢复。
- 每个被接受的请求有唯一 `RequestId`，最终一定进入 `Ready` 或明确的终态错误；结果只能
  消费一次，再次消费返回 `kAlreadyTaken`。
- `StopAndJoin` 原子地停止接收新命令，完成全部已接受命令，在工作线程清理所有客户端、
  关闭 Runtime、退出工作线程，再由调用方 `join`。它**不**等待调用方取走结果；停止后结果
  仍可在执行器对象有效期内读取。
- 结果中的 `pending_commit` 仍然是**待宿主应用的文本**，不是已写入任何应用。

## 2. 文件与职责

```
platforms/windows/engine_executor/
  CMakeLists.txt                 由 platforms/windows/engine_bridge 引入，链接 P3.4 桥接库
  engine_executor.h              公共契约：状态、命令、结果、容量、生命周期
  engine_executor.cc             状态机、工作线程、FIFO、容量、结果存储、异常终结
  engine_backend.h               内部接缝：工作线程上的 Runtime/Session/Controller 适配
  engine_executor_internal.h     测试工厂与真实后端工厂（非公共能力）
  real_engine_backend.cc         真实 Runtime/Session/Controller 的所有权与调用

tests/windows/engine_executor/
  CMakeLists.txt                 5 条具名 CTest
  main.cc                        模式分派与退出码
  test_support.{h,cc}            固定夹具、部署准备、真实 ToUnicodeEx 翻译、命令辅助
  controlled_backend.{h,cc}      受控测试后端（不加载 rime.dll，不做真实策略）
  scheduler_tests.cc             调度层：状态机、FIFO、容量、超时、停止竞争、异常
  real_engine_tests.cc           真实英文引擎、线程归属、raw/preedit/commit、并发、失败
```

职责边界：

| 对象 | 负责 | 明确不负责 |
| --- | --- | --- |
| `EngineExecutor` | 生命周期、唯一工作线程、FIFO、容量、请求 ID、结果存储、停止排空 | 决策规则、宿主文本、候选 UI、TSF |
| `EngineBackend` | 工作线程上的运行时/客户端操作接口 | 队列、容量、调用方结果所有权 |
| `RealEngineBackend` | `RimeRuntime`、`RimeSession`、`InputController` 的独占所有权 | 线程创建、队列、结果发布 |
| `EngineExecutorTestFactory` | 测试注入受控后端 | 产品调用路径 |
| 调用方 | 提交值命令、等待/取走自己的结果 | 直接调用 Runtime/Session/Controller |

`EngineExecutor(ExecutorOptions)` 是产品构造入口，内部创建真实后端。测试工厂只在
`engine_executor_internal.h` 中声明，且只被测试目标使用。

## 3. 为什么必须是专用线程

P3.4 的 `RimeRuntime` 在 `Start` 时记录 owner thread。之后：

- `RimeSession` 的每个方法先检查 `IsOwnerThread()`，不是 owner 就返回 `kWrongThread`，
  不调用引擎（`rime_session.h:13-16`；`rime_session.cc` 的 `ProcessKeyOnce`、
  `CaptureState`、`ReadCommit`、`Close`）；
- `InputController` 的每个方法同样拒绝非 owner 线程（`input_controller.h:39-43`）；
- `RimeRuntime::Shutdown` 在非 owner 线程返回 `kWrongThread`，且仍有存活 Session 时返回
  `kLiveSessionsRemain`（`rime_runtime.h:33-35`）；
- `RimeRuntime::Start` 通过进程级 `g_runtime_slot_held` 保证每个进程至多一个已启动
  Runtime（`rime_runtime.cc`，P3.4 契约 §2）。

因此执行器不是“把 Controller 包一层锁”，而是把**所有**引擎操作搬到唯一工作线程，并让
调用方只接触值。这样既不需要修改 P3.4 的 owner-thread 规则，也不会让多个线程同时进入
librime 的进程级单例。

## 4. 公共契约

### 4.1 标识与状态

```cpp
struct RequestId { std::uint64_t value; };  // 0 = invalid
struct ClientId  { std::uint64_t value; };  // 0 = invalid

enum class ExecutorState {
  kNotStarted, kStarting, kRunning, kStopping, kStopped, kFailed
};
```

`RequestId` 和 `ClientId` 都只增不复用。`ClientId` 在 create 命令**被接受时**分配，因此
调用方可以立即把它用于后续命令；create 失败时该 ID 永久失效。

### 4.2 命令

| `CommandKind` | 提交方法 | 工作线程上的动作 | 结果 payload |
| --- | --- | --- | --- |
| `kCreateClient` | `TryCreateClient` | `CreateSession` + `InputController` 构造（含一次只读状态捕获） | `CreateClientOutcome` |
| `kEvaluate` | `TryEvaluate` | 只调用 `InputController::Evaluate`；**不** `process_key`、**不**读状态/commit | `EvaluateOutcome` |
| `kDispatch` | `TryDispatch` | 重新评估当前缓存，至多一次 `process_key`，随后一次状态/commit 读取 | `DispatchCommandOutcome` |
| `kAcceptCandidate` | `TryAcceptCandidate` | 至多一次 `select_candidate_on_current_page`，随后状态/commit 读取 | `CandidateCommandOutcome` |
| `kRefreshState` | `TryRefreshState` | 只读 `get_status/get_context/get_input` | `RefreshOutcome` |
| `kCloseClient` | `TryCloseClient` | 销毁 Controller，关闭 Session | `CloseClientOutcome` |

没有通用线程池、插件任务、任意 `std::function` 回调或 COM 调度器。Evaluate 仍然是纯
决策：它的存在是为了让未来调用方能在**工作线程的缓存**上询问“这个键是否可派发”，而不是
把 Controller 暴露给调用线程。

### 4.3 提交结果与命令终态

`SubmitStatus`：

```
kAccepted, kNotRunning, kQueueFull, kClientUnknown, kClientNotOpen,
kInvalidRequest, kInternalFailure
```

- `kNotRunning`：Starting/Stopping/Stopped/Failed；
- `kQueueFull`：容量已满；调用方不被阻塞；
- `kClientUnknown`：从未发出过该 ClientId；
- `kClientNotOpen`：客户端正在关闭、已关闭或创建失败；
- `kInternalFailure`：入队分配失败；本次没有接受任何请求。

`CommandStatus`：

```
kOk, kClientUnavailable, kSessionCreateFailed, kSessionCloseFailed,
kControllerFailed, kInternalFault, kExecutorFailed, kInvalidRequest
```

`kOk` 只表示“命令执行了并且 payload 有意义”，**不**表示按键被宿主吃掉，也不表示文本已
写入应用。`kInternalFault` 是单条命令的异常终结；`kExecutorFailed` 用于执行器级故障。
两者必须分开：前者不杀死执行器，后者属于整个实例。

## 5. 生命周期状态机

```
                 Start()                        worker: Runtime::Start ok
 [NotStarted] ----------------> [Starting] ---------------------------> [Running]
      |                              |                                     |
      | Start invalid / thread fail  | start failed                        | StopAndJoin
      v                              v                                     v
   [Failed] <---- cleanup failed --[Stopping] <------------------------- (stop requested)
      ^                              |
      |                              | worker: drain accepted commands,
      |                              | close clients, Runtime::Shutdown,
      +------------------------------+ worker exits
                                     |
                                     v
                                 [Stopped]
```

规则：

1. **单次启动**：`Start` 每个对象只尝试一次。第二次 `Start`（包括失败/停止后）返回
   `kAlreadyStarted`；需要再次启动时创建新实例，并遵守 P3.4 的进程级排他规则。
2. **显式启动**：`Start` 不得在 `DllMain` 中调用；本轮不把执行器加入 DLL 激活路径。
3. **Start 失败**：工作线程报告真实的 `RuntimeStartStatus`，执行器进入 `kFailed`，线程
   退出；`StopAndJoin` 返回 `kExecutorFailed`。失败对象不可重试。
4. **StopAndJoin**：
   - 在锁内把状态改为 `kStopping` 并置 `stop_requested_`，此后 `TrySubmit` 返回
     `kNotRunning`；
   - 如果 Stop 在 `Starting` 期间胜出，工作线程在 `Runtime::Start` 返回后**不得**把状态
     改回 `kRunning`；`Start` 返回 `kStoppedDuringStart`，准入保持关闭；
   - 工作线程继续执行**所有已接受**命令（队列不因停止而丢弃）；
   - 队列为空后，在工作线程销毁所有客户端、关闭 Runtime；
   - 工作线程发布 `kStopped`（清理干净）或 `kFailed`（清理失败），然后退出；
   - 调用方 `join`。不 detach，不 `TerminateThread`。
5. **不等待结果被取走**：Stop 不检查容量占用，也不要求结果被消费。停止后结果仍在
   `EngineExecutor` 对象内，直到对象析构。
6. **自等待**：`StopAndJoin` 先比较 Win32 线程 ID。如果调用者是工作线程，立即返回
   `kSelfJoinRefused`，不尝试 `join`。本轮没有用户回调，正常产品路径不会触发；测试用受控
   后端的 hook 显式验证它不会静默死锁。
7. **析构**：析构函数调用 `StopAndJoin`（若尚未停止），保证不留下 joinable 线程。析构
   与显式 Stop 不应从两个线程并发调用；`StopAndJoin` 自身用 `stop_mutex_` 串行化重复
   调用，并用 `join_mutex_` 保证只有一个线程执行 `join`。
8. **重复 Stop**：第一次干净停止返回 `kOk`；之后返回 `kAlreadyStopped`。启动失败后的
   第一次 Stop 返回 `kExecutorFailed`，之后返回 `kAlreadyStopped`。

## 6. 线程、客户端、请求生命周期图

### 6.1 线程

```
caller thread(s)                         worker thread (the only engine owner)
-----------------                        --------------------------------------
TryCreateClient  -- value command -->    queue (mutex, FIFO)
TryDispatch      -- value command -->        |
TryEvaluate      -- value command -->        v
TryAcceptCandidate                           pop under lock, unlock
TryRefreshState                              RimeRuntime::CreateSession
TryCloseClient                               RimeSession::ProcessKeyOnce
                                             InputController::Evaluate/Dispatch
WaitForResult  <-- ready/notify --           CaptureState / ReadCommit
TakeResult     <-- copied value --           Client close / Runtime::Shutdown
                                             publish result under lock
```

- 真实引擎调用期间**不持队列锁**；锁只覆盖出队、状态表更新、结果发布。
- 工作线程是 `RimeRuntime::Start` 的调用者，因此 owner-thread 检查全部通过；任何意外
  在错误线程发生的调用会被 P3.4 明确拒绝，而不是静默执行。
- 结果中的 `executing_thread_id` 是执行该命令的 Win32 线程 ID；`ExecutorRuntimeReport`
  另存 Runtime `Start`/`Shutdown` 的线程 ID。测试断言它们全部相同且不等于调用线程。

### 6.2 客户端

```
create accepted          create executed ok
[Pending] -----------> [Open] -----------------> normal commands
    |                     |  \
    | create failed       |   close accepted
    v                     v        |
 [Failed]              [Closing] --+--> worker executes close
                                  |
                                  v
                              [Closed] / [Failed]
```

- `Pending` 是 create 已接受但尚未执行；后续命令允许入队，FIFO 保证它们在 create 之后。
- create 失败后，已排队的后续命令会得到 `kClientUnavailable`；新提交得到
  `kClientNotOpen`。
- close 一旦被接受就把状态改为 `Closing`，**后续**针对该客户端的新命令立即被拒绝
  （`kClientNotOpen`）；此前已接受的命令按 FIFO 先执行。
- 客户端关闭**不**删除它尚未取走的完成结果。

### 6.3 请求

```
TrySubmit: state Running? capacity free? client allowed?
   no -> explicit SubmitStatus, no RequestId allocated
   yes -> allocate RequestId, copy value command, enqueue, occupied++

worker: pop -> execute (exactly once) -> store terminal CommandResult, outstanding--

caller: WaitForResult -> kReady / kTimedOut / kUnknownRequest / kAlreadyTaken
        TakeResult    -> move result out, erase, occupied-- (exactly once)
```

## 7. FIFO、容量与结果

### 7.1 FIFO

- 一个全局 `std::deque<EngineCommand>`，一个消费者。
- 命令在 `Submit` 的锁内追加；同一调用线程对同一客户端的提交顺序就是执行顺序。
- 不同客户端、不同线程的命令按入队顺序交错；每个客户端只看到自己的状态，不共享
  Session。
- `repeat_count` 只是 P3.3 的元数据，一条 Dispatch 命令最多一次 `process_key`；执行器
  不展开重复计数。

### 7.2 容量

```
occupied = queued + executing + completed_unconsumed
```

- 容量在 `ExecutorOptions::capacity` 中设置，`Start` 时校验 `> 0`；默认 32。
- `TrySubmit` 在 `occupied >= capacity` 时返回 `kQueueFull`，**不阻塞**。
- 已接受请求不会因为新请求而被丢弃，结果不会被覆盖（`results_.emplace` 不覆盖）。
- `TakeResult` 成功后才 `occupied--`。停止后不再接受新命令，因此停止后的取走只释放
  额度，不会产生新的提交。
- `capacity()` 返回 `CapacityUsage{capacity, occupied, queued, executing,
  completed_unconsumed}`，用于测试和诊断，不暴露任何请求内容。

测试覆盖三种占满形态：执行中、排队中、已完成未取走（见 §12 的 capacity 用例）。

### 7.3 结果与等待

- 每个 `RequestId` 只执行一次，只有一条终态结果。
- `WaitForResult(request, timeout)` 只等待，不消费、不取消、不重试。超时后请求继续执行，
  之后仍可 `Wait`/`Take`。
- `TakeResult` 消费一次。消费后再取返回 `kAlreadyTaken`；从未接受过的 ID 返回
  `kUnknownRequest`；仍在排队/执行返回 `kNotReady`。
- “已取走”不需要无界 tombstone 集合：请求 ID 只在接受时连续分配。一个
  `id <= last_submitted` 且既不在 `outstanding_` 也不在 `results_` 中的请求，只能是已
  取走。
- `pending_commit` 是引擎队列中的待宿主文本。执行器不调用 `ITfRange`、`SetText` 或
  `SendInput`；`commit` 读取仍由 P3.4 的 `RimeSession::ReadCommit` 做一次。

## 8. 关闭与停止竞争

### 8.1 close 被接受后的规则

1. 提交 close：在锁内把客户端状态改为 `Closing`，把 close 命令追加到队列。
2. 此后任何针对该客户端的新命令在**提交阶段**返回 `kClientNotOpen`，不会入队。
3. 此前已接受的命令仍在队列中 close 之前，按 FIFO 执行；它们的执行结果照常发布。
4. close 命令执行时销毁 Controller、关闭 Session；结果存入结果表。
5. close 结果本身和此前的已接受结果都不因关闭而删除。

### 8.2 Stop 与提交竞争

`StopAndJoin` 与 `TrySubmit` 用同一把 `mutex_` 串行化准入判断：

- 如果 `TrySubmit` 先拿到锁，命令被接受，Stop 之后的排空会执行它并发布终态结果；
- 如果 Stop 先拿到锁，状态变为 `kStopping`，`TrySubmit` 返回 `kNotRunning`，没有
  RequestId 被分配；
- 不存在“命令在队列里但 Stop 不执行”或“Stop 完成后还有工作线程在跑”的中间状态。

真实引擎竞争测试用 4 个提交线程、barrier 和 condition_variable 让提交与 Stop 真正重叠；
对每个 `kAccepted` 的 ID，Stop 后必须能 `Wait` 到 `kReady` 并 `Take` 一次；对每个拒绝，
必须有明确的 `SubmitStatus`。测试不靠 `sleep` 猜测时序。

## 9. 跨线程边界与隐私

**只传值**。命令和结果都不包含：

- `ITfContext*`、`ITfThreadMgr*` 或任何 COM 接口指针；
- 借用的 Rime 指针或 `RimeStatus/RimeContext/RimeCommit` 内部缓冲；
- 调用线程栈地址或引用。

`KeyEventSnapshot`、`KeyText`、`EngineState`、`EngineCommit` 都是 P3.3/P3.4 定义的
调用方拥有值；执行器在持锁入队时复制它们，结果在 worker 上复制后发布。

生产代码**不记录**按键文本、raw input、preedit、commit、剪贴板、焦点字段名或应用标识。
诊断字符串只包含状态名、计数、阶段名和 schema 标识；不包含输入文本。测试只使用仓库内
固定夹具字符。

`InputPermission::kForbidden` 和 `kUnknown` 的字段在 P3.3 决策层就被拒绝；执行器不会为
它们调用 `process_key`，因此也不会产生组合或学习副作用。密码字段/一次性验证码字段的
排除仍是 P3.3 的字段级规则，不是执行器的线程/队列能替代的。

## 10. 真实引擎接入

`RealEngineBackend` 是唯一持有 `RimeRuntime` 的对象：

- `StartRuntime` 在工作线程调用 `RimeRuntime::Start`，记录启动线程；
- `CreateClient` 调用 `RimeRuntime::CreateSession`，成功后构造 `InputController`；
- Evaluate/Dispatch/AcceptCandidate/Refresh 直接调用 P3.4 的对应方法；
- `CloseClient` 先销毁 Controller，再调用 `RimeSession::Close`；
- `CleanupAllClients` 在工作线程销毁所有剩余 Controller/Session，记录存活 Session 数，
  然后调用 `RimeRuntime::Shutdown`；
- `RuntimeInfoCopy`/`ApiCountsCopy` 在 Stop 时由工作线程复制，调用方只拿到值。

真实引擎测试观察到的关键事实（固定夹具 `epocanva_test_english`）：

| 事实 | 证据 |
| --- | --- |
| Runtime `Start`/`Shutdown` 在工作线程 | `ExecutorRuntimeReport.start_thread_id == shutdown_thread_id == worker_thread_id()` |
| 每条命令都在工作线程 | 每个 `CommandResult.executing_thread_id` 等于 worker ID，且不等于调用线程 |
| 两个调用线程、两个客户端不串扰 | X 线程得到 `h/he/hel`，Y 线程得到 `a/ap/app`；Refresh 分别返回 `hel`/`app` |
| 同一客户端顺序稳定 | 连续提交三条 dispatch，结果中的 raw 依次为 `h`、`he`、`hel` |
| 重复 Evaluate 不派发 | 三次 Evaluate 后 Refresh raw 为空；真实 `process_key` 只在 dispatch 后增加 |
| 一命令一次派发 | 一次 dispatch 结果 `dispatched-handled`，`engine_key_call_made == true`，API 计数与命令数一致 |
| 字母/退格/Return/Space | raw `hel`；BackSpace 回到 `he`；Return commit `hel`；空组合 Space commit 一个空格 |
| 显式候选 | 选中候选 1 后 preedit `hello`；随后 Space commit `hello ` |
| 边界/快捷键/禁止许可 | 逗号在组合中 `kDeferredCompositionBoundary` 且无 `process_key`；Ctrl+A `kNotDispatched`；forbidden 字段 `kNotDispatched` |
| commit 只取一次 | Return 结果有 `hel`；下一次 Refresh 不读 commit；下一次 dispatch 无旧 commit |
| 关闭后拒绝新命令 | close 被接受后 `TryDispatch` 返回 `kClientNotOpen` |
| 关闭不破坏已有结果 | 先接受 dispatch 的结果不取走，close 后再取，结果和 raw 仍完整 |
| 容量满载 | capacity=2 时第三次提交 `kQueueFull`；取走一个后恢复接受 |
| 停止后排空 | Stop 期间已接受的请求全部得到终态结果；停止后无活 Session |
| 进程级排他 | 执行器运行时直接 `RimeRuntime::Start` 返回 `kAnotherRuntimeActive`；第二个执行器启动失败；第一个停止后可重新启动独立 Runtime |

## 11. 初始化失败与清理

`startup-failure` 用例覆盖：

1. **缺失 DLL**：`Start` 返回 `kRuntimeStartFailed`；`RuntimeReport` 中
   `runtime_start_status == kDllLoadFailed`；执行器状态 `Failed`；没有命令被接受；
   `StopAndJoin` 返回 `kExecutorFailed`。
2. **相对 DLL 路径**：`kInvalidOptions`，在加载前拒绝。
3. **baseline DLL（无 English 模块）**：`kEnglishModuleMissing`；P3.4 的失败路径已经
   调用一次 `finalize` 并释放 DLL；`initialize_calls == 1`、`finalize_calls == 1`、
   API 配对平衡。随后正确 DLL 的新执行器可以启动并干净停止，证明失败没有永久占用进程
   级槽位。
4. **清理失败（受控后端）**：`RuntimeShutdownStatus::kFinalizeFailed` 使 Stop 返回
   `kCleanupFailed`、状态 `Failed`；这与单条命令的 `kInternalFault` 明确区分。

正常停止后：

- 所有客户端在工作线程销毁；
- `Runtime::Shutdown` 在工作线程执行；
- `RuntimeInfo.live_session_count == 0`；
- `ApiCallCounts::balanced()`（status/context/commit/schema config 获取与释放配对，
  create/destroy session 配对）；
- 工作线程已 join，状态 `Stopped`。

## 12. 构建、测试与证据

### 12.1 配置与构建

使用本机已确认的 Visual Studio 2022 Professional + 其自带 CMake 3.31.6：

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test

... cmake.exe --build build/windows-engine-bridge/test --config Release
```

结果：`EXIT=0`，`epocanva_engine_executor.lib`、`engine_executor_tests.exe` 均生成，
无 `/W4` warning。

### 12.2 CTest

```bat
ctest --test-dir build/windows-engine-bridge/test -C Release ^
      --output-on-failure --no-tests=error ^
      -R "windows\.(engine|executor)\."
```

结果：`11/11` 通过，0 失败（P3.4 六条 + P3.5 五条）。P3.5 五条：

| 用例 | 检查数 | 结果 |
| --- | ---: | --- |
| `windows.executor.scheduler_lifecycle` | 323 | PASS |
| `windows.executor.scheduler_can_fail` | 1 次故意失败 | exit 14，无 RESULT PASS |
| `windows.executor.real_engine` | 492 | PASS |
| `windows.executor.stop_race` | 789 | PASS |
| `windows.executor.startup_failure` | 14 | PASS |

调度层用例用受控后端，**不加载 rime.dll**；真实引擎用例使用真实 English DLL。两者是不同
的命令行模式、不同的 CTest 名称、不同的日志。

### 12.3 回归

以下既有构建直接运行 CTest，不重建第三方库：

| 套件 | 过滤 | 结果 |
| --- | --- | --- |
| P3.1 TSF 生命周期 | `tsf\.` | 3/3 通过 |
| P3.2 文本边界 | `windows\.text\.` | 4/4 通过 |
| P3.3 键盘决策/翻译 | `windows\.input\.` | 5/5 通过 |
| P3.4 engine bridge | `windows\.engine\.` | 6/6 通过 |
| English 集成（librime-english） | `english\.` | 13/13 通过 |
| component probe | `librime\.` | 5/5 通过 |
| 根工程（Debug） | `infrastructure\.\|english\.core\.` | 7/8 通过；1 条既有性能预算失败 |

根工程的 `english.core.spelling_cost_trial_on_pilot_lexicon` 在 Debug 构建下报
`worst single call 45136.3 us vs 25000.0 us budget`，单独重跑仍为 48010 us。该测试是既有
性能试验，二进制和 English core 源码本轮均未修改；这是本机当前负载/构建配置下的既有
预算失败，不是 P3.5 的回归。本文不把它记为绿色，也不修改阈值来绕过它。

## 13. 需求覆盖矩阵

| # | 要求 | 覆盖位置 |
| --- | --- | --- |
| 1 | Runtime 在工作线程启动/关闭 | `real_engine` report 线程 ID、initialize/finalize 计数 |
| 2 | 所有真实引擎调用线程 ID 一致 | 每个 CommandResult 的 `executing_thread_id` + report |
| 3 | 两个调用线程操作不同客户端不串扰 | `real_engine` X/Y barrier 用例 |
| 4 | 同一客户端事件顺序稳定 | `real_engine` X/Y 的逐条 raw；`scheduler` FIFO |
| 5 | 重复 Evaluate 不派发、一次命令一次派发 | `real_engine` 三次 Evaluate + 一次 Dispatch；`scheduler` |
| 6 | 字母、退格、Return、Space、显式候选 | `real_engine` 全套固定夹具 |
| 7 | 快捷键/禁止许可/边界不错误派发 | `real_engine` Ctrl+A / forbidden / comma；`scheduler` 策略结果 |
| 8 | commit 仅取得一次 | `real_engine` Return → Refresh → 下一次 Dispatch |
| 9 | 关闭后拒绝新命令 | `scheduler` + `real_engine` close 用例 |
| 10 | 关闭不破坏已接受结果 | `scheduler` close2；`real_engine` 未取走的 dispatch |
| 11 | 容量满明确拒绝、无覆盖/静默丢弃 | `scheduler` capacity/capacity2；`real_engine` capacity |
| 12 | 取走结果后额度恢复 | `scheduler` capacity；`real_engine` capacity |
| 13 | 等待超时不取消、不自动重放 | **调度层确定性验证**：受控后端在 Dispatch 内持门，30 ms 超时后释放，请求只执行一次；真实引擎套件只验证有限等待成功，不声称强制出超时 |
| 14 | Stop 与提交竞争：拒绝或终态 | `scheduler` stop-drain；`real_engine` stop_race（4 线程 barrier） |
| 15 | Stop 后无活 Session、无工作线程 | `real_engine` report + state + join 返回 |
| 16 | 缺失 DLL/错误初始化完整失败退出 | `startup_failure` 缺失/相对/baseline |
| 17 | 重复 Stop 行为明确 | `scheduler`、`real_engine`、`startup_failure`；`scheduler` 另有 Stop 胜出 Starting 期的准入测试 |
| 18 | 现有 Runtime 排他规则仍有效 | `real_engine` 运行中直接 Runtime/第二执行器 + 停止后恢复 |

**第 13 项的边界必须明确**：执行器的 `WaitForResult` 实现与后端无关，调度层用受控后端
确定性地制造“已接受但尚未完成”的请求，验证超时后请求仍执行一次、结果仍可取。真实引擎
套件没有可控阻塞点，因此没有在真实 `process_key` 执行期间强制制造超时；它验证的是同一
结果存储/容量路径在真实引擎上的成功路径。把这两者混为一谈会是不诚实的。

## 14. 明确未实现 / 未验证

- 没有 TSF sink、没有 `AdviseKeyEventSink`、没有注册输入法、没有 `DllCanUnloadNow`
  生产逻辑修改。
- 没有宿主文本编辑、没有 `ITfRange`/`SetText`/`SendInput`；`pending_commit` 不是“已
  应用”。
- 没有候选 UI、没有焦点切换处理、没有键盘吞键策略、没有 TSF 多线程集成。
- 没有通用线程池、插件任务系统、任意 `std::function` 回调、COM 调度器或命令取消语义。
- **未来接入 DLL 的集成尚未验证**。接入时必须满足：
  1. 执行器的 `StopAndJoin` 必须在 **loader lock 之外**调用；不能在 `DllMain` 中
     `Start`/`Stop`/`join`。
  2. 工作线程存活期间不得允许模块卸载：`DllCanUnloadNow` 必须返回 `S_FALSE`，或模块
     引用计数必须覆盖工作线程生命周期；本轮不改这个生产逻辑。
  3. 未来若 TSF 的调用线程需要“立即决策”，仍必须通过命令/结果或等待接口，不能把
     `InputController` 指针交给宿主线程。
  4. 第 13 项的真实引擎超时强制验证需要一个可控的引擎阻塞点；本轮没有为了测试把测试
    钩子放进生产后端，因此该项只在调度层完成。
- 既有 English 性能预算失败（§12.3）未修复、未调整阈值；它不属于本轮范围。

## 15. 未来接入 DLL 的检查清单

1. 在 DLL 激活/停用路径中，执行器的 `Start`/`StopAndJoin` 必须在 loader lock 之外。
2. 工作线程存在时，`DllCanUnloadNow` 必须拒绝卸载；或者由宿主显式保持模块引用。
3. 不在 `DllMain` 中创建/join 线程，不在 `DllMain` 中等待引擎。
4. 宿主线程只使用 `EngineExecutor` 的提交/等待/取走接口；不传递 `ITfContext*`、
   `ITfThreadMgr*` 或 Rime 内部指针。
5. 密码/一次性验证码等字段仍然在提交前由 P3.3 的 `InputPermission` 拒绝；执行器不会
   因为“已经在线程里”而绕过隐私规则。
