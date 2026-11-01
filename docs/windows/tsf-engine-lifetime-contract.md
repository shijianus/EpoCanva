# Windows TSF 引擎生命周期契约（P3.6：Broker、租约与卸载护栏）

本文描述 `platforms/windows/tsf/` 与 `tests/windows/tsf_lifecycle/hosted/` 在 P3.6
**实际实现并实际验证**的内容。主题只有一个：把 P3.5 的单线程 `EngineExecutor` 接入
TSF 的激活/停用与模块卸载判断，并保证启动、停止、失败和清理窗口内的状态是明确的。

本轮**不**接受真实键盘命令，**不** AdviseKeyEventSink，**不**写宿主文本，**不**做候选
UI，**不**注册输入法。绿色测试只证明生命周期算术与卸载判断，不证明能打字。

## 1. 结论先行

- 一个进程内只有一个 `EngineHostBroker`，它最多拥有一个 `EngineExecutor`。
- 第一个租约创建并启动执行器；后续租约共享它；最后一个租约释放时，在释放线程上显式
  `StopAndJoin`（停止接收、排空、清理客户端、关闭 Runtime、退出 worker、join）。
- 停止完成后执行器对象被销毁；之后再激活会创建一个**新的** `EngineExecutor` 实例，
  不会对已停止实例调用 `Start`。
- 启动只有一个发起者；同一配置的并发 `Acquire` 等待这一次启动并共享结果；启动失败时
  所有等待者收到明确失败，不做隐式重试。
- `Stopping` 期间的新 `Acquire` 立即拒绝；失败或清理未安全结束时进入 `Failed`，拒绝
  后续租约，且 `DllCanUnloadNow` 永久 `S_FALSE`。
- TSF `Activate` / `ActivateEx` 是事务式的：先取新 ThreadMgr 引用、再取新引擎租约，
  两者都成功后才切换状态；失败时旧激活状态完全保留。同一个 ThreadMgr 重复激活先
  AddRef 后 Release，不会出现悬空引用。
- `DllCanUnloadNow` 只读四个计数器/标志（活 COM 对象、服务器锁、host-active、
  host-failed），不加锁、不 join、不初始化引擎、不做文件 I/O。清理窗口由
  host-active 覆盖到执行器对象销毁之后。
- 没有使用永久 `GET_MODULE_HANDLE_EX_FLAG_PIN`，也没有把正常停止交给 DllMain。

## 2. 源码、构建变体与边界

### 2.1 新增/修改的源码

```
platforms/windows/tsf/
  engine_host.h                Broker、租约、配置、诊断值的公共内部契约
  engine_host.cc               状态机、唯一 executor、锁顺序、测试门闩
  module_state.h/.cc           增加 host-active / host-failed 两个原子护栏
  text_service.h/.cc           事务式 Activate/Deactivate、异常析构策略

tests/windows/tsf_engine_host/
  main.cc                      Broker/租约测试（直接链接生产 broker 源码）
  CMakeLists.txt               2 条 CTest

tests/windows/tsf_lifecycle/hosted/
  test_seam_abi.h              固定 C/POD 测试 ABI
  test_seam.cc                 测试 DLL 内的接缝实现
  epocanva_tsf_hosted_test.def 测试 DLL 导出表（含私有测试函数）
  hosted_main.cc               托管生命周期验证程序
  CMakeLists.txt               测试 DLL + 9 条 CTest
```

### 2.2 两个 DLL 变体

| 变体 | 目标 | 宏 | 内容 |
| --- | --- | --- | --- |
| 原骨架 | `epocanva_tsf`（`platforms/windows/tsf` 独立项目） | 无 | P3.1 的 COM 生命周期；不链接引擎；原有 3 条测试继续运行 |
| 托管测试 | `epocanva_tsf_hosted_test`（由 `platforms/windows/engine_bridge` 引入） | `EPOCANVA_TSF_ENGINE_HOST=1`、`EPOCANVA_TSF_TEST_SEAM=1` | 同一组生产 TSF 源文件 + `engine_host.cc`；链接 `epocanva_engine_executor`；测试接缝只在此变体 |

托管测试 DLL 的 `.def` 含 `DllGetClassObject`、`DllCanUnloadNow`、两个注册导出（W3-B
起，只断言存在，测试不调用）和固定的 `EpoCanvaTest*` 测试函数；产品 `epocanva_tsf.def`
导出**恰好四个**名字：`DllGetClassObject`、`DllCanUnloadNow`、`DllRegisterServer`、
`DllUnregisterServer`。注册策略在 `platforms/windows/tsf/registration.*`；B 层用内存
注册表后端测试，真实 HKLM/HKCU 写入只在显式授权的 C 层会话发生。

**产品配置与部署已完成（W1–W3-A），系统注册尚未执行（W3-B）。** 产品从自身模块目录取得
数据/`rime.dll`，用户数据在 `%LOCALAPPDATA%\EpoCanva`；W3-B 实现注册导出但本机没有安装
器工具链，也没有运行系统注册。托管测试 DLL 仍是显式测试变体，不是可部署的输入法。

## 3. Broker 状态机与不变量

状态：`Idle / Starting / Running / Stopping / Failed`。例外路径（启动失败且清理干净）
回到 `Idle`；清理失败或异常弃租进入 `Failed`，该状态是进程生命周期内的终态。

### 3.1 启动

- 只有看到 `Idle` 并成功把状态改为 `Starting` 的线程是启动者；`generation` 递增。
- 启动者在**不持有 broker 互斥锁**时创建 `ExecutorOptions`、构造 `EngineExecutor`、
  调用 `Start()`。`Start` 的 worker 自己调用 `Runtime::Start`。
- 同一配置的并发 `Acquire` 在 `Starting` 上等待；不同配置立即 `kConfigMismatch`，
  不等待、不替换正在启动/运行的环境。
- 启动成功后，启动者先登记自己的租约，再 `notify_all`；等待者醒来在 `Running` 上各自
  登记租约。整个过程只有一次 Runtime 初始化。
- 启动失败后，启动者在不持锁时 `StopAndJoin`，把执行器对象移出并在不持锁时销毁；
  `DllCanUnloadNow` 的 host-active 标志直到销毁完成才清除。
- 失败清理干净（worker 已 join、Runtime shutdown 状态为 `kOk`/`kNotStarted`、
  无残留 session）→ `Idle`，后续激活可创建新执行器。所有等待者收到
  `kStartFailed`，不自动重试。
- 失败清理不干净 → `Failed`、`cleanup_failed=true`、`ModuleSetHostFailed(true)`；
  等待者收到 `kCleanupFailed`。此状态不会回到 `Idle`。

### 3.2 共享与释放

- 租约是 `EngineLease`，移动语义、析构自动 Release；`Release()` 幂等，第二次返回
  `kAlreadyReleased`，handle 不再进入 broker。
- broker 侧用唯一 id 集合保存活动租约。未知 id 的 `ReleaseLease` 返回
  `kAlreadyReleased` 并只增加 `release_underflows` 诊断，不会把计数减成负数。
- 非最后租约释放只减少集合；最后租约释放把状态改为 `Stopping`，在释放线程上执行
  `StopAndJoin`。`StopAndJoin` 返回后先发布最终 `ExecutorRuntimeReport`，再经过
  cleanup 窗口，把执行器对象移出并销毁，最后才把状态改为 `Idle` 并清除 host-active。
- `Stopping` 期间新 `Acquire` 立即 `kStopping`；不会被排队到停止结束后重新开放。
- `Failed` 期间新 `Acquire` 立即 `kFailed`。

### 3.3 锁顺序与线程

```
text service state_mutex_  ->  broker mutex  ->  (unlocked) EngineExecutor lifecycle
```

- broker mutex 保护状态、配置快照、租约集合和计数器。
- `EngineExecutor::Start`、`StopAndJoin` 以及执行器对象销毁都不在 broker mutex 内执行。
- 执行器的 worker 从不回调 broker、COM 或租约持有者。它只运行 Runtime/Session 操作。
- 租约/服务对象只接触值诊断；没有任何 `EngineExecutor`、`RimeRuntime`、`RimeSession`、
  `InputController` 或裸 DLL 句柄暴露给调用方。
- `ITfThreadMgr` / `ITfContext` 不进入 broker 或 worker。TSF 线程只在租约边界调用 broker。

### 3.4 异常弃租

服务对象在**没有先 Deactivate** 的情况下被最后一次 `Release` 时，析构函数不能安全地
`StopAndJoin`：这次 Release 可能发生在 loader lock 内。防御策略是：

- 释放本对象持有的 ThreadMgr 引用；
- `EngineLease::Abandon()`：从 broker 租约集合移除，**不**停止、**不** join；
- broker 进入 `Failed`，设置 `abandoned` 和模块 host-failed；
- 执行器与 worker 保留到进程结束，`DllCanUnloadNow` 永远 `S_FALSE`。

这是异常路径，不是正常实现。正常路径必须在 `Release` 前调用 `Deactivate`，由最后一个
租约在明确线程上停止并 join。ThreadMgr 的 `Release` 在未知 loader lock 状态下的具体行为
本轮**未验证**，因此不声称它“安全”；只声称本 DLL 不会再等待 worker 线程。

## 4. TSF 激活/停用

### 4.1 Activate / ActivateEx

两个入口共用 `ActivateInternal`。顺序：

1. 拒绝空 `ITfThreadMgr`（`E_INVALIDARG`）。
2. 读取进程配置副本；没有配置 → `E_UNEXPECTED`。
3. 向 broker `Acquire` 新租约（阻塞条件仅为一次正在进行的启动）。失败直接返回，**旧激活
   状态未触碰**：
   - `kStartFailed` / `kCleanupFailed` → `E_FAIL`
   - `kInternalFailure` → `E_OUTOFMEMORY`
   - `kStopping` / `kFailed` / `kConfigMismatch` / 配置非法 → `E_UNEXPECTED`
4. 对新 ThreadMgr 调用 `AddRef`。
5. 在对象 mutex 内一次性切换 `thread_manager_`、client id、flags、active 和新租约；
   旧租约被 move 出。
6. 在对象 mutex 外释放旧租约，再释放旧 ThreadMgr 引用。

因此：

- 失败激活保留原有效激活（包括原 ThreadMgr 引用和原租约）。
- 同一个 ThreadMgr 重复激活先 AddRef 后 Release，不可能因“先 Release 后 AddRef”悬空。
- 成功的重复激活租约数保持 1：先 Acquire 到 2，释放旧租约回到 1。
- 不向执行器传任何 COM 指针。

### 4.2 Deactivate

- 在对象 mutex 内把 ThreadMgr 指针和租约 move 出，清空 client id/flags/active。
- 在 mutex 外先释放租约（最后一个租约会停止/join），再 Release ThreadMgr。
- 幂等：未激活时只是成功 no-op，不会重复释放租约（不会触发 broker 下溢）。
- 不在 DllMain 中调用；DllMain 仍只记录/清除 HINSTANCE。

## 5. 模块卸载护栏

`module_state` 现在有五个判断量：

| 量 | 含义 | 何时清零 |
| --- | --- | --- |
| `g_live_objects` | 类工厂 + 文本服务活对象 | 最后一个对象析构 |
| `g_server_locks` | `LockServer(TRUE)` | 匹配的 `LockServer(FALSE)` |
| `g_host_active` | 引擎启动/运行/停止/执行器销毁窗口 | 清理完全结束、执行器销毁之后 |
| `g_host_failed` | 清理失败/异常弃租的单向闩锁 | 永不清除（进程生命周期） |
| `g_registration_active` | W3-B 注册/反注册的整个注册表序列 | 序列返回前 |

`DllCanUnloadNow` 只返回 `ModuleCanUnload() ? S_OK : S_FALSE`。`ModuleCanUnload` 只读上述
五个值：不加锁、不 join、不初始化、不调用引擎、不做文件 I/O。注册窗口计数保证
`DllRegisterServer`/`DllUnregisterServer` 写注册表期间卸载判断为 `S_FALSE`
（注册本身不创建 COM 对象、不持有服务器锁）。

关键顺序：

- 启动：先设置 host-active，再进入 `Starting`/调用 `Start`；失败清理完成前不清除。
- 停止：进入 `Stopping` 时 host-active 已为 1；`StopAndJoin` 完成、最终报告发布、cleanup
  窗口结束、执行器对象销毁之后，才把状态改为 `Idle` 并清除 host-active。
- 失败：host-failed 置 1 且永不清除；即使活对象和锁都归零，`DllCanUnloadNow` 仍
  `S_FALSE`。
- 不会先把活对象计数降到 0 再继续执行仍需 DLL 的清理：正常停止发生在 Deactivate 内，
  服务对象此时仍计数；即使宿主并发释放对象，host-active 仍覆盖整个停止/销毁窗口。

DllMain 没有被改成清理器；没有永久 pin DLL；正常路径靠显式 `StopAndJoin` + 对象销毁，
不是受控泄漏。

## 6. 测试接缝

测试接缝只在 `epocanva_tsf_hosted_test.dll` 中编译（`EPOCANVA_TSF_TEST_SEAM`），并只在
其 `.def` 中导出。ABI 是固定 C 结构：

- `EpoCanvaTestSetEngineConfig`：四条 `const wchar_t*` + capacity；DLL 立即复制为
  `std::filesystem::path`，调用方随后可释放原缓冲区；不接受 STL 所有权。
- `EpoCanvaTestGetDiagnostics`：固定 POD，包含状态、租约数、等待者数、实例 id、启动/
  停止计数、Runtime 初始化/finalize 次数、线程 id、模块计数、host 标志和 can-unload。
- `EpoCanvaTestSetStartGate` / `SetStopGate` / `SetCleanupGate`：各两个不透明 HANDLE；
  DLL 在固定位置 signal `entered` 并等待 `release`。没有回调、没有 user data、没有内部
  对象指针。
- `EpoCanvaTestSetForceCleanupFailure`：受控故障注入，仅用于证明“清理失败不得假装
  Idle / 不得允许卸载”的闩锁路径；真实清理失败路径与它共用状态转移代码，但本项明确
  标记为受控后端/故障注入，不与真实引擎清理成功混报。
- `EpoCanvaTestLeaseDoubleRelease`：固定 POD 报告 acquire/第一次释放/第二次释放/重复 id
  的状态和租约计数，不返回租约指针。

测试目录全部来自 CMake 传入的绝对路径并落在 `build/<...>/tsf-host-run/`；不访问
`%APPDATA%`、真实用户词典目录或任何个人绝对路径。没有通用插件/依赖注入框架。

## 7. 测试结果（本机实际执行）

### 7.1 托管 TSF 测试（真实 ThreadMgr + 真实 English rime.dll）

| CTest 用例 | 检查数 | 结果 |
| --- | ---: | --- |
| `tsf.hosted.single_service` | 52 | PASS |
| `tsf.hosted.shared_services` | 38 | PASS |
| `tsf.hosted.multi_sta` | 32 | PASS |
| `tsf.hosted.unload_window` | 46 | PASS |
| `tsf.hosted.cleanup_failure` | 21 | PASS（受控清理失败注入） |
| `tsf.hosted.startup_failure` | 33 | PASS（缺 DLL + 缺 English 模块） |
| `tsf.hosted.lease_underflow` | 19 | PASS |
| `tsf.hosted.original_com_contract` | 51 | PASS（原 P3.1 COM 契约） |
| `tsf.hosted.suite_can_fail` | 6（1 次故意失败） | exit 14，无 RESULT PASS |

覆盖：

1. 一个服务激活只启动一个 Runtime（`initialize_calls == 1`）。
2. 两个服务共享一个执行器（`executor_instances_created == 1`，租约 2）。
3. 一个停用后另一个仍可 `ActivateEx`。
4. 最后停用后 `finalize_calls == 1`、worker 线程 id 相同、执行器对象销毁。
5. 停止后再次激活使用新实例（实例 id 前进、`start_attempts` 增加）。
6. 失败激活（配置不一致）不破坏原激活。
7. 同一 ThreadMgr 重复激活安全（租约仍 1、Runtime 仍 1）。
8. 两个 STA 线程各自创建真实 ThreadMgr/服务，共享同一 broker。
9. 每个 COM 对象只在所属线程使用；测试中主线程只调用 DLL 导出诊断/卸载判断，不触碰
   另一线程的 ThreadMgr 或 service 指针。
10. Starting / Stopping / cleanup 三个门闩内 `DllCanUnloadNow == S_FALSE`。
11. 全部对象释放后 `S_OK`。
12. 缺 DLL / 缺 English 模块均完整清理，随后新配置可启动新执行器。
13. Stopping 期间服务级 `Activate` 返回 `E_UNEXPECTED`，不产生新启动。
14. 租约重复释放：handle 第二次返回 `kAlreadyReleased`；broker 重复 id 只增加诊断，
    计数保持 0。
15. 原类工厂、LockServer、错误 IID、五轮创建/销毁全部通过。

竞争用例（`shared_services`、`multi_sta`、`unload_window`、broker `broker_lifecycle`）
以 `--repeat until-fail:5` 重复 5 次，全部通过；所有窗口由 event/barrier 控制，没有用
sleep 猜时序。

### 7.2 Broker 直接测试（不经过 COM）

`windows.tsf_host.broker_lifecycle`：162 checks / 0 failed。覆盖单启动、共享租约、
最后释放停止+join、新实例、配置不匹配、Stopping 拒绝、启动失败重试、
缺 English 模块重试、重复释放、单启动者+等待者、受控清理失败闩锁。
`windows.tsf_host.broker_can_fail`：exit 14，无 RESULT PASS。

### 7.3 回归

| 套件 | 结果 |
| --- | --- |
| P3.1 TSF 骨架（`build/windows-tsf/test`） | 3/3 通过 |
| P3.2 文本边界（`build/windows-text/test`） | 4/4 通过 |
| P3.3 键盘决策/翻译（`build/windows-input/test`） | 5/5 通过 |
| P3.4 + P3.5 + P3.6 broker（`build/windows-engine-bridge/test`） | 13/13 通过 |
| P3.6 hosted（同一构建目录） | 9/9 通过 |
| English 正式入口（`build/english-pilot/test`，`^english\.`） | 15/15 通过 |
| component probe（`build/librime-component-probe/probe`） | 5/5 通过 |

### 7.4 性能门：Debug 失败，Release 单独测量

**Debug 失败事实（未隐藏、未改阈值）**

根工程 Debug 全量 CTest：7/8 通过，`english.core.spelling_cost_trial_on_pilot_lexicon`
失败：

```
[en-core] pilot-bench worst        67023.5 us for 'zqelectroencephalographicaaaaaaa'
got    : 67023.500000 us vs 25000.000000 us
```

单独重跑此前记录为 60937.7 us（本次全量运行为 67023.5 us），均超过固定的 25,000 us
预算。该测试本轮未修改，阈值未提高，测试未删除、未默认跳过。

**Release 测量（只用于区分构建配置，不覆盖 Debug 失败）**

同一根工程 Release 构建、同一输入、同一预算：

```
[en-core] pilot-bench index        42356 rows, read and built in 16896.7 us
[en-core] pilot-bench worst        391.1 us for 'zqelectroencephalographicaaaaaaa'
RESULT PASS
```

**归因核对（有限）**

- 性能测试是根工程 `epocanva_english_core_tests.exe` 的 `--pilot-bench` 模式，只链接
  English core；根 `CMakeLists.txt` 不包含 TSF/executor/engine_bridge。该进程不会初始化
  `EngineExecutor`，也不存在本轮引入的 worker。
- 执行器清理代码：`EngineExecutor::~EngineExecutor()` 调用 `StopAndJoin()`；
  `Impl::StopAndJoin()` 通过 `JoinWorker()` 对 `worker_` 执行 `join()`；没有 detach，
  没有 terminate。现有 scheduler 套件 323 checks 通过，包含 stop 排空、self-join 拒绝、
  start/stop 竞争和 cleanup-failure 终态；hosted/broker 测试另行验证最后租约后
  `finalize_calls == 1`、状态 `Idle`、执行器对象不存在。
- 没有观察到与本轮执行器相关的 worker 残留或资源增长；但这只是短进程生命周期观察，
  不是泄漏/内存增长测量。
- 输入与预算来源：`build/english-pilot/epocanva_en_pilot.tsv`（42,356 行，既有生成物，
  本轮未重建）；`tests/english_core/main.cc` 固定 200 次重复、13 个输入、k=3、
  25,000 us 预算。Debug 是未优化构建，Release 同机同输入为 391.1 us。

**结论**：Debug 性能失败仍是未归因到本轮改动的既有问题；本轮不宣称全绿。Release
测量通过，但不能覆盖 Debug 失败事实。

## 8. 未完成项、风险与未验证范围

1. **没有键盘接入**：没有 `ITfKeystrokeMgr::AdviseKeyEventSink`、没有 `OnKeyDown`、
   没有 `ITfComposition`、没有宿主文本编辑、没有候选 UI。`pending_commit` 的概念甚至
   未进入本层；激活只启动/停止 Runtime。
2. **没有注册/部署**：DLL 不导出注册函数，不写注册表/profile/category；托管测试 DLL
   只是显式测试变体。真实产品配置来源与部署流程未实现。
3. **异常析构未验证范围**：如果宿主违反 COM 契约，在未 Deactivate 的情况下释放最后
   引用，本 DLL 不 join、弃租、永久阻止卸载，worker 保留到进程结束。ThreadMgr 的
   Release 在 loader lock 下的具体行为未验证；本层只保证不等待 worker。
4. **进程级 Runtime 排他**：librime 的进程级单例规则未变；broker 通过“先停止/join 再
   创建新执行器”满足它，不支持两个不同配置的执行器并存。
5. **性能失败未归因**：Debug 下的 English core pilot-bench 超预算，与本轮代码路径无
   链接关系，但未进一步定位（本轮任务限定为有限归因核对）。
6. **真实应用内验证不存在**：没有把 DLL 放进任意宿主进程、没有用真实应用打字。本文
   所有绿色结果都只针对生命周期和测试夹具。

## 9. 复现命令（本机实际使用）

```bat
:: 1) broker + hosted test DLL + 全部 P3.4/P3.5/P3.6 测试
cmake -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test
cmake --build build/windows-engine-bridge/test --config Release
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error ^
      -R "windows\.(engine|executor|tsf_host)\." --output-on-failure
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error ^
      -R "tsf\.hosted\." --output-on-failure

:: 2) 原 TSF 骨架（不开启托管）
cmake --build build/windows-tsf/test --config Release
ctest --test-dir build/windows-tsf/test -C Release --no-tests=error ^
      -R "^tsf\.lifecycle\." --output-on-failure

:: 3) English / probe 回归
ctest --test-dir build/english-pilot/test -C Release --no-tests=error ^
      -R "^english\." --output-on-failure
ctest --test-dir build/librime-component-probe/probe -C Release --no-tests=error ^
      -R "librime\.component_probe\." --output-on-failure

:: 4) 根工程 Debug（预期 7/8，性能门失败）
ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure

:: 5) Release 性能测量（单独记录）
cmake --build build/vs2022-x64-debug --config Release
ctest --test-dir build/vs2022-x64-debug -C Release --no-tests=error -V ^
      -R "english\.core\.spelling_cost_trial_on_pilot_lexicon"
```

## 10. 提交

- 第一个提交：`6ccc23fff5a1be6f3e8535ea786c2a4deecde688`
  `feat(windows): add shared engine host leases`
- 第二个提交：引入本文、事务式激活和卸载护栏；
  `feat(windows): bind engine lifetime to TSF activation`。提交自身不能包含自己的 SHA，
  完整 SHA 由本轮交付报告给出。

## 11. W2-B-R 增补：启动顺序、TSF 持有的回调与模块卸载（已实现）

本节记录 W2-B-R 实际修改并验证的行为；前文 P3.6 的 broker/租约规则不变。

### 11.1 引擎启动只在键接收器注册成功之后发起

`ProductTextController::Start` 只建立 STA 控制器、唤醒通道和异步启动等待者；它不再在
`ActivateInternal` 内直接调用 `ProductEngineBootstrap::RequestStart()`。新的
`RequestEngineStart()` 由 `text_service.cc` 在 `AdviseKeyEventSink` 成功之后调用。

原因是一个真实缺陷：严格产品 DLL 在未注册时 `AdviseKeyEventSink` 被拒绝，旧顺序已经
把后台引擎启动发出；失败的 `Activate` 回滚了控制器，却没有、也不能在 STA 上等待/取消
那次启动。启动完成后引擎运行而没有所有者，`host_active` 保持 1，`DllCanUnloadNow`
永久 `S_FALSE`；宿主退出时后台执行器线程仍在 DLL 内，进程以访问冲突结束。

启动仍然异步：`Activate` 在字面模式立即返回，`RequestEngineStart` 只投递请求。失败的
引擎启动由 `engine_state/failure` 报告，`DllCanUnloadNow` 在后台线程退出前保持
`S_FALSE`，之后回到 `S_OK`。

### 11.2 异步编辑会话的引用只转移一次

`CompositionEditSession::Submit` 在 `phr_session == TF_S_ASYNC` 时把**构造引用转移**到
`State::pending_callback`，不再额外 `AddRef`。旧代码保留了构造引用又加了一个，完成的
每个异步编辑泄漏一个 `ITfEditSession` 回调；这些回调由 TSF 持有，导致模块永远不可
卸载，并可能在 DLL 卸载后才被 TSF 释放（调用已解除映射的 vtable）。

`PendingResult`/`Close`/`State` 析构各释放一次；同步路径仍按原样释放。

### 11.3 交给 TSF 的 COM 对象计入模块存活

`platforms/windows/tsf_edit/module_lifetime.h`（header-only，按镜像各自计数）由产品 DLL
在 `ProductTextController::Start` 安装 `ModuleAddObject/ModuleReleaseObject`。以下对象
在存在期间计入 `DllCanUnloadNow`：

| 对象 | 位置 |
| --- | --- |
| `ITfEditSession`（组合编辑） | `EditSessionCallback` |
| `ITfEditSession`（选择读取） | `SelectionReadCallback` |
| `ITfCompositionSink` | `CompositionSink` |
| `ITfEditSession`（候选位置读取） | `RectReadCallback` |
| `ITfCandidateListUIElementBehavior` | `CandidateUiElement` |
| `ITfUIElementSink` | `UiElementSink` |

`DllCanUnloadNow` 的回答仍是四个计数器的只读判断，不 join、不加锁、不触碰 TSF。

### 11.4 宿主的卸载契约（受控测试宿主）

`tests/windows/tsf_product` 的顺序改为：

1. `Deactivate`，再释放 TIP / sink / class factory；
2. 拆除 TSF 宿主（Pop 文档、`CoUninitialize`），让 TSF 释放它仍持有的回调/sink；
3. 以 10 s 为上限等待 `DllCanUnloadNow == S_OK`；
4. 只有 `S_OK` 才 `FreeLibrary`；`S_FALSE` 时保留模块并打印
   `[unload] ... stayed S_FALSE ... (not forced)`，绝不强制卸载；
5. 释放后按地址确认镜像确实消失。

同时修复了 `TestHost::PumpUntil` 的一个活性缺陷：它以“消息队列为空”为结束条件，而产品
在等待写授权时每 10 ms 发一次唤醒消息，导致 500 ms 切片永不返回。现在使用**整体截止
时间**并在每个切片最多派发 64 条消息。

验证：
- `product.integration.sink_unload_contract`：队列中的编辑会话使 `DllCanUnloadNow`
  为 `S_FALSE`；授予锁后 TSF 释放回调，门变为 `S_OK`，陈旧会话不写文档，随后安全卸载。
- `product.integration.sink_startup_lifecycle`：启动阻塞期间停用、旧启动晚于下一次激活、
  旧启动失败不被新激活采纳、后台线程退出后门打开。
- `product.integration.sink_strict_unregistered`：Advise 失败不再留下引擎启动，门在
  宿主拆除后打开，进程正常退出。
- `product.integration.sink_capacity_continue` 与 `sink_degraded_keys` 结束时都断言模块
  已卸载。
