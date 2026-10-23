# Windows 引擎桥接契约（P3.4：输入控制器与真实 librime Session）

本文档描述 `platforms/windows/engine_bridge/` **当前实际是什么**、它如何加载并持有
librime、一个 Session 如何被创建和关闭、P3.3 的决策如何变成一次真实的 `process_key`
调用、结果如何被复制与释放、线程归属如何拒绝违规调用，以及哪些路径**没有**被验证。

本轮范围是：**受控的 Runtime/Session 生命周期**、**把 P3.3 决策接到真实英文引擎**、
**把 preedit/候选/comment/commit 复制成调用方拥有的 UTF-16 结果**。本轮**不**注册输入法、
**不**创建 `ITfKeyEventSink`、**不**调用 `AdviseKeyEventSink`、**不**修改宿主文本、
**不**做候选 UI。

## 1. 目录与职责

```
platforms/windows/engine_bridge/
  CMakeLists.txt                 独立顶层项目，静态 CRT (/MT)
  rime_runtime.{h,cc}            唯一的 DLL/API/进程级生命周期
  rime_runtime_internal.h        Runtime 与 Session 共享的内部状态（非公开头）
  rime_session.{h,cc}            一个 RimeSessionId 的创建、按键、选词、读取、关闭
  engine_update.{h,cc}           调用方拥有的文本/候选/commit 副本与偏移映射
  input_controller.{h,cc}        P3.3 决策的执行与结果状态
tests/windows/engine_bridge/
  CMakeLists.txt                 6 条具名 CTest
  main.cc                        runtime / state / 两个 can-fail 模式
  controller_tests.cc            dispatch / guards 模式
  test_support.h                 固定夹具准备、检查输出、路径比较（测试专用）
```

职责边界：

| 对象 | 负责 | 明确不负责 |
| --- | --- | --- |
| `RimeRuntime` | 显式 DLL 加载、API 表检查、`setup`/`initialize`/`finalize`、模块检查、一次性部署、Session 计数 | 按键决策、preedit/commit 语义、宿主文本 |
| `RimeSession` | 一个 session 的创建、schema 选择与核验、一次 `process_key`、一次 `select_candidate_on_current_page`、只读状态读取、一次 commit 读取、关闭 | 进程初始化、决策、候选 UI |
| `InputController` | 读取缓存状态、调用 P3.3 `DecideKey`、把允许的决策映射为一次引擎按键、把结果分类 | 直接调用 Win32 `ToUnicodeEx`、注册 sink、编辑宿主 |
| `engine_update` | UTF-16 副本、UTF-8→UTF-16 严格转换、preedit 偏移映射、候选/comment/commit 数据 | 任何引擎调用或策略判断 |

没有最终产品 C ABI、插件框架、IPC 服务或调度器。

## 2. 进程级共享状态：为什么只能有一个 Runtime

librime 的 service、session 表、deployer、registry 和 module manager 都是进程级单例：

- `Service::instance()` 是函数内静态对象（`third_party/librime/src/rime/service.cc:196-203`）；
- `RimeInitialize` 调用 `SetupDeployer`、`LoadModules` 和 `Service::StartService`
  （`src/rime_api_impl.h:51-56`）；
- `RimeFinalize` 调用 `StopService`、`Registry::Clear()` 和
  `ModuleManager::UnloadModules()`（`src/rime_api_impl.h:58-63`）；
- `LoadModules` 按名字装载并调用模块的 `initialize()`（`src/rime/setup.cc:33-40`）。

因此：

- **每个进程至多一个已启动的 `RimeRuntime`**。第二次 `Start` 返回
  `kAnotherRuntimeActive`，不会再次 `initialize`；
- Session 的创建/销毁**不**调用 `setup`、`initialize` 或 `finalize`；
- `finalize` 只在 `RimeRuntime::Shutdown` 中调用，并且**仍有存活 Session 时拒绝**
  （`kLiveSessionsRemain`），不会卸载 DLL；
- 初始化失败路径（例如缺少英文模块）会调用一次 `finalize` 再 `FreeLibrary`，并释放
  进程级槽位，使后续正确的 Runtime 仍能启动。

测试中有一个**准备用** Runtime：它只负责把固定夹具部署到受控目录，随后完整
`Shutdown`；主 Runtime 在它之后启动，**不再部署**。两者串行，不同时存在。这不是每个
Session 各自初始化；主 Runtime 仍然只初始化一次并服务两个 Session。

## 3. DLL 的显式路径与真实身份

`RuntimeOptions::rime_dll_path` 必须是绝对路径。加载方式：

```cpp
LoadLibraryExW(path, nullptr,
               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
```

含义：

- 不使用 `LoadLibraryW("rime.dll")`，因此不搜索当前工作目录或 `PATH`；
- `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` 让依赖从被加载 DLL 自己的目录解析；
- 不接受相对路径（`kInvalidOptions`），不依赖 cwd；
- 目标系统为 Windows 10 1607+（与 P3.3 `ToUnicodeEx` bit 2 的最低版本一致）。

加载后的身份核验：

1. `GetModuleFileNameW` 取得实际加载路径；
2. 用 `GetFullPathNameW` 归一化并用 `CompareStringOrdinal(..., TRUE)` 与请求路径比较；
3. `GetModuleHandleExW(FROM_ADDRESS, rime_get_api)` 证明该符号属于同一个模块；
4. `rime_get_api()` 返回非空、`data_size > 0`；
5. 逐个检查本轮必需的公开槽位（见 §4）。

`RuntimeInfo` 保存请求路径与实际路径、librime 版本、API 完整性、模块状态、初始化/
终结计数、Session 计数与部署计数。**实际加载身份不是从文件名猜的**；测试断言
`loaded_dll_path` 与命令行给出的绝对路径一致。

## 4. 公开 API 与模块

本轮只使用公开 C API。必需槽位：

```
setup, initialize, finalize
create_session, find_session, destroy_session
process_key, select_candidate_on_current_page
get_context, free_context
get_status, free_status
get_commit, free_commit
get_input
select_schema, schema_open, config_close
get_version, find_module
```

若请求部署，还要求 `deploy_config_file`、`deploy_schema`。

`RimeTraits`：

| 字段 | 值 / 来源 |
| --- | --- |
| `shared_data_dir` | 调用方给的绝对 ASCII 目录 |
| `user_data_dir` | 调用方给的绝对 ASCII 目录 |
| `log_dir` | 调用方给的绝对 ASCII 目录 |
| `prebuilt_data_dir` | `nullptr`，由 librime 派生 `<shared>/build`（`src/rime/setup.cc:56-61`） |
| `staging_dir` | `nullptr`，由 librime 派生 `<user>/build`（`src/rime/setup.cc:62-65`） |
| `app_name` | `rime.epocanva.engine_bridge` |
| `modules` | `{"default", ["levers",] "epocanva_english", nullptr}` |

字符串和模块数组由 `RuntimeCore` 持有，`RimeTraits` 只保存指向它们的指针；
`RuntimeCore` 由 Runtime 和每个 Session 共同持有，初始化期间不会被释放。

模块检查：

- `find_module("core"/"dict"/"gears"/"default")` 必须非空；
- `find_module("epocanva_english")` 必须非空；
- 通过该模块公开的 `get_api()` 读取 `EpoCanvaEnglishApi`，检查 `data_size` 至少覆盖
  本桥接已知的字段，并调用 `get_counters()`；
- `module_initialized > 0` 证明英文模块的 `initialize()` 真的运行过。

诚实边界：`find_module` 证明**注册**，不是“已装载”的独立查询。`core/dict/gears`
是否真的可用，由后续创建 Session、选择 schema、得到真实候选来行为验证；
`levers` 是否可用，由准备 Runtime 的部署返回值和编译产物验证。

英文模块通过 librime 自己的 `RimeTraits.modules` → `LoadModules` 机制装载
（`src/rime/setup.cc:33-40`）；本桥接**不**调用 `rime::Registry` 跨 DLL 注册对象。

## 5. 目录、ASCII 与不触碰用户数据

- `shared_data_dir`、`user_data_dir`、`log_dir` 必须绝对、存在、是目录；
- 三者必须是 ASCII。原因不是排版：`RimeTraits` 的路径是窄字符串，本层不猜测其编码，
  非 ASCII 路径直接 `kInvalidOptions`；
- 不读取 `%APPDATA%`、`%LOCALAPPDATA%`、用户已有 Rime 目录或当前工作目录；
- 不下载、不扫描语言包；
- Runtime 只在调用方明确请求时部署**明确列出的**文件（见 §6）。

产品代码中没有 `printf`、`std::cout`、`LOG`/`DLOG`、`OutputDebugString`。测试程序不打印任何用户
输入：除固定夹具字符串（`h/e/l/hel/hello/...`）外，只输出环境标识（布局 KLID、DLL 路径、
librime 版本、受控 run-root）、API/状态名和失败诊断；其中没有会话文本、焦点字段名或
应用标识。

## 6. 测试配置准备与运行时使用分开

准备阶段由**独立 Runtime** 完成：

1. 把 `tests/fixtures/english_completion/` 的 `default.yaml`、schema、54 行词表复制到
   受控 `<run-root>/shared/`；
2. 把 `tests/fixtures/english_spelling/` 的 schema 与 16 行词表也复制到同一个受控
   `shared/`（词表放在 `shared/lexicon/`，与 schema 的 `lexicon_path` 对齐）；
3. 以 `deployment` 请求启动准备 Runtime，调用一次 `deploy_config_file("default.yaml",
   "config_version")` 和两次 `deploy_schema(...)`；
4. 验证 `<user>/build/` 下确实出现三份编译产物；
5. 完整 `Shutdown`。

主 Runtime 使用同一个已准备的 `shared/user/log` 目录，**没有** deployment 请求。
测试在创建 Session、输入按键前后统计 `<user>/build` 的文件数，必须不变；主 Runtime 的
`deploy_calls` 必须是 0。因此：

- 部署不是每次按键或每次 Session 创建时发生；
- 测试配置准备在 Runtime 使用之前完成；
- 运行时不会自动重建词表或扫描目录。

部署本身是同步的：`Deployer::RunTask` 直接调用任务的 `Run`
（`src/rime/deployer.cc:28-40`），没有维护线程、没有 sleep、没有轮询。

## 7. 单位、副本与偏移

### 7.1 单位

| 名称 | 单位 | 说明 |
| --- | --- | --- |
| `utf8_byte_length` | UTF-8 码元（字节） | librime C API 的字符串单位 |
| `utf16_code_units` | UTF-16 码元 | TSF/宿主方向；`wchar_t` 在此工具链为 16 位 |
| `cursor_utf16_code_unit_offset` | UTF-16 码元偏移 | 从对应文本开头算起 |
| `selection_*_utf16_code_unit_offset` | UTF-16 码元偏移 | 同上 |
| `RimeComposition.length/cursor_pos/sel_start/sel_end` | UTF-8 字节偏移 | 指向 `preedit` |
| `RimeSessionId` | 不透明 id | 0 是无效值（`src/rime/service.h:20`） |

`EngineText` 同时保存 UTF-16 副本、UTF-8 字节长度和该文本自己的 P3.2 边界表。
所有文本在 `free_status`/`free_context`/`free_commit` 之前复制；返回后不再持有 DLL 内
指针。`get_input()` 的返回值没有 free 槽（`rime_api.h:426-430`），在
`src/rime_api_impl.h:1062-1070` 返回 `ctx->input().c_str()`，本层立即复制。

### 7.2 preedit 位置映射

`RimeComposition` 的 `length`、`cursor_pos`、`sel_start`、`sel_end` 是 **preedit 的
UTF-8 字节偏移**，不是 raw input 的偏移。`MapPreeditOffsets(preedit_utf8, ...)` 用
**preedit 自己的**边界表映射：

- 负数：`cursor_pos < 0` → `kNoCursor`；`sel_start < 0 && sel_end < 0` →
  `kNoSelection`；只有一个为负 → `kSentinelMismatch`；
- 越界 → `kOutOfRange`；
- 落在 UTF-8 多字节序列中间 → `kInsideSequence`；
- `start > end` → `kOrderInvalid`；
- 合法零长度选择（`start == end == 文本末尾`）→ `kMapped`。

不 clamp、不取最近边界。`Preedit` 的 `caret_pos` 初值是 `string::npos`
（`src/rime/composition.cc:27-28`），`sel_end` 也会短暂使用 `npos` 再解析；本层按上述
哨兵语义处理，而不是把它们当成巨大的偏移。

测试显式覆盖了：ASCII、2 字节字符、4 字节代理对、嵌入式 NUL、非法 UTF-8、负数哨兵、
单边哨兵、越界、序列内部、`start > end`。另有真实引擎证据：选中 `hello` 后 raw input
仍是 `hel`（3 字节），preedit 是 `hello`（5 个 UTF-16 码元），映射出的 cursor/selection
是 5；如果误用 raw input 的表，只会得到 3。

### 7.3 候选、comment 与 commit

- 候选文本和 comment 逐个复制为 UTF-16；`comment == nullptr` 是空字符串，不是错误；
- `EngineState.candidates` 是**当前页**顺序，`page_size/page_no/is_last_page/
  highlighted_candidate_index_on_page` 同页；
- `EngineCommit` 是引擎队列中**待宿主应用**的事件。`present == true` **不**表示任何
  应用已经写入、显示或持久化；
- `ReadCommit()` 调用一次 `get_commit`；该 API 成功时清空队列
  （`src/rime_api_impl.h:309-324`）。本层只在 Dispatch/AcceptCandidate 之后调用它，
  只读 `RefreshState()` **不**调用 `get_commit`。

## 8. 线程规则（第一版：单线程受控桥接）

`RimeRuntime::Start` 记录 owner thread。以下操作都检查归属，违规时返回
`kWrongThread` / `false`，**不**触碰引擎：

- `CreateSession`、`Shutdown`；
- Session 的 `ProcessKeyOnce`、`SelectCandidateOnce`、`CaptureState`、`ReadCommit`、
  `Close`；
- Controller 的 `Evaluate`、`Dispatch`、`AcceptCandidate`、`RefreshState`、
  `SetInputEnabled`、`SetPermission`。

`Shutdown` 在 owner thread 上、但仍有存活 Session 时返回 `kLiveSessionsRemain`，
不调用 `finalize`、不卸载 DLL。`RimeSession` 析构函数同样检查线程：错误线程上**不**调用
`destroy_session`，而是保留引擎 session 与存活计数，使后续 `Shutdown` 继续拒绝；这是
为了防止在无法安全关闭时悄悄卸载 DLL。若 Runtime 在仍有 Session 时被析构，析构函数
也只在 owner thread 上尝试 `Shutdown`；无法安全关闭时保留 DLL 不卸载（受控泄漏），
而不是制造 use-after-free。测试只验证违规**方法调用**被拒，不制造跨线程析构。

这是**单线程受控桥接**，不是最终 TSF 多线程方案。TSF 的 sink 线程模型、异步编辑、宿主
锁和跨线程 Session 归属，属于后续轮次，本层没有声称已解决。

## 9. 决策执行

### 9.1 Evaluate / CanDispatch

`Evaluate` 只读取 Controller 缓存的 `EngineState` 和 P3.3 的 `DecideKey`：

- 不调用 `process_key`；
- 不调用 `get_status`/`get_context`/`get_input`/`get_commit`；
- 不改变 preedit、候选或 commit。

测试用三次相同 `Evaluate('h')` 比较决策相等，检查 `process_key` 计数和
`state_refreshes` 都不变，并用真实 `CaptureState()` 证明 raw input 仍为空。
`CanDispatch` 只是对返回值的纯判断。

缓存只在 `Dispatch`、`AcceptCandidate`、`RefreshState` 之后更新；`RefreshState` 是显式的
只读查询，供宿主在外部状态变化后调用。

### 9.2 Dispatch

`Dispatch(event, text)` **重新**调用 `Evaluate`，使用执行时刻的缓存、权限和 enabled
状态，而不是调用方先前拿到的决策对象。测试先 `Evaluate` 得到“可派发”，再把权限改为
`forbidden`，`Dispatch` 必须拒绝且不调用引擎。

只有 `decision.is_engine_candidate()` 为真才执行一次 `process_key`：

| disposition | keysym | mask |
| --- | --- | --- |
| `kEngineTextCandidate` | `ascii_code_point` 原值 | 0 |
| `kEngineSpaceCandidate` | `0x0020` | 0 |
| `kEngineBackSpaceCandidate` | `0xff08` | 0 |
| `kEngineEnterCandidate` | `0xff0d` | 0 |
| `kEngineEscapeCandidate` | `0xff1b` | 0 |

keysym 常量来自 `third_party/librime/include/X11/keysymdef.h:124,128,132,527`；
可打印 ASCII 的 keysym 等于码点。文本直接使用 P3.3 已经翻译出的 scalar，**不**从 VK
重新推导，**不**再次应用 Shift/Caps。`repeat_count` 是元数据，不展开；一次回调最多一次
`process_key`。

不送入引擎的情况：快捷键/AltGr（Control、Alt、Windows 任一）、key-up、权限
forbidden/unknown、input disabled、engine not ready、Shift+特殊键、组合区边界需求。

`kCompositionBoundaryRequired` 返回 `kDeferredCompositionBoundary`：不调用引擎，
**不丢弃**已有组合，也不声称已经处理。测试在 `hel` 后送入逗号，验证 raw/preedit/候选
全部不变，随后继续输入 `l` 仍得到 `hell`。

### 9.3 AcceptCandidate

显式接受当前页的候选：先检查 index 在当前缓存菜单内，再调用**一次**
`select_candidate_on_current_page`。选择当前页版本而不是
`select_candidate`，是因为 `RimeContext.menu` 是页数据，缓存中的候选索引是页内索引；
`select_candidate` 接受的是菜单全局索引。

`AcceptCandidate` 之后同样读取一次 state 和一次 commit。它不提交、不编辑宿主；下一步
（如 Space）才由引擎的 P2.5 规则提交所选候选。测试对比：

- `hel` 后直接 Space → `hel `（默认原文）；
- `hel` 后显式接受候选 1（`hello`）再 Space → `hello `。

## 10. 结果状态

`DispatchStatus`：

| 值 | 含义 |
| --- | --- |
| `kNotDispatched` | 没有调用 `process_key`；策略拒绝或未就绪 |
| `kDeferredCompositionBoundary` | 有组合区但不能决定顺序；没有引擎调用，组合保持原样 |
| `kDispatchedUnhandled` | 调用了一次 `process_key`，返回 False；state 已读取 |
| `kDispatchedHandled` | 调用了一次 `process_key`，返回 True；state 已读取 |
| `kDispatchedStateFailed` | 已调用引擎，但随后的状态读取/转换或 commit 读取失败 |

`DispatchOutcome` 携带 `state_status`、`commit_status`、`state`、`pending_commit`、
`engine_key_call_made`、`engine_reported_handled` 和 `decision`。已调用引擎后**不会**
简单返回 false；失败被命名，避免宿主重放同一个键。

`AcceptCandidate` 使用 `CandidateStatus`：`accepted`、`rejected-by-engine`、
`out-of-range`、`not-ready`、`wrong-thread`、`state-failed`。

## 11. 测试与真实证据

### 11.1 构建

```sh
CMAKE="<VS 2022>/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
CTEST="<同上目录>/ctest.exe"

"$CMAKE" -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test \
      -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
"$CMAKE" --build build/windows-engine-bridge/test --config Release
"$CTEST" --test-dir build/windows-engine-bridge/test -C Release -N -R "^windows\.engine\."
"$CTEST" --test-dir build/windows-engine-bridge/test -C Release -R "^windows\.engine\." \
      --no-tests=error --output-on-failure
```

实际结果：

| 步骤 | 退出码 | 证据 |
| --- | --- | --- |
| 配置 | 0 | `build/windows-engine-bridge-configure-r2.log` |
| 构建 r11 | 0，无 warning | `build/windows-engine-bridge-build-r11.log` |
| `-N` 过滤 | 0，`Total Tests: 6` | `build/windows-engine-bridge-ctest-list-r1.log` |
| CTest | 0，6/6 Passed | `build/windows-engine-bridge-ctest-r6.log` |

### 11.2 直接运行

实际英文 DLL：`build/librime-english/rime/install/lib/rime.dll`；实际版本 `1.17.0`。<br>
实际键盘布局：KLID `00000804`（中文简体）。

| 模式 | 检查数 | 结果 | 日志 |
| --- | --- | --- | --- |
| `runtime` | 113 | 0 failed，退出 0 | `build/windows-engine-bridge-runtime-direct-r6.log` |
| `state` | 33 | 0 failed，退出 0 | `build/windows-engine-bridge-state-direct-r3.log` |
| `dispatch` | 173 | 0 failed，退出 0 | `build/windows-engine-bridge-dispatch-direct-r6.log` |
| `guards` | 124 | 0 failed，退出 0 | `build/windows-engine-bridge-guards-direct-r4.log` |
| `self-check-fail` | 1 | 1 failed，退出 **14** | 不含 `RESULT PASS` |
| `self-check-fail-dispatch` | 1 | 1 failed，退出 **14** | 不含 `RESULT PASS` |

关键真实输出（来自固定夹具）：

- `runtime`：加载路径等于请求的绝对路径；librime 版本非空；必需槽位齐全；
  `core/dict/gears/default` 已注册；`epocanva_english` 已装载；
  `initialize_calls == 1`；第二次 Runtime 返回 `kAnotherRuntimeActive`；
  两个 Session 分别输入 `h` 和 `a`，互不影响；存活 Session 时 `Shutdown` 返回
  `kLiveSessionsRemain`；关闭后 `Shutdown` 成功，`finalize_calls == 1`；
  无缺失/错误 DLL、无非 ASCII 路径、无缺少 schema 的误通过；日志目录为空；
  API acquire/free 配平。
- `dispatch`：三次 `Evaluate('h')` 不调用 `process_key`、不刷新状态、raw 仍为空；
  一次 `Dispatch` 后 raw/preedit 都是 `h`，候选 0 是 `h`，候选 1 是 `hello`；
  `hel` 的候选恰好 `hel / hello / help / helmet`；大写 `H` 后 raw 是 `H`、`He`，
  候选 1 是 `Hello`；BackSpace → `he`，Escape 清空组合，Enter 提交 `hel`，
  Space 提交 `hel `，空组合 Space 提交 ` `；候选 1 显式接受后 Space 提交 `hello `；
  同一 commit 第二次读取得到 `no-commit`；`RefreshState` 不读 commit；
  `repeat_count=5` 只产生一个 `h`；最终 `process_key` 见证数与各 Controller 的
  实际调用数一致。
- `guards`：逗号边界返回 `deferred-composition-boundary`，raw/preedit/候选不变，
  继续输入仍得到 `hell`；Ctrl/Alt/Win/Ctrl+Alt、release、forbidden、unknown、
  disabled、null session 都不增加 `process_key`，raw 仍为 `hel`；两个 Controller
  交错得到 `hel` 与 `app` 且互不串扰；所有跨线程方法（包括 `Shutdown`、
  `CreateSession`、`Close`、`ReadCommit`）都被拒绝且不改变 raw/计数。

### 11.3 回归

| 回归项 | 命令要点 | 退出码 | 结果 | 日志 |
| --- | --- | --- | --- | --- |
| P3.1 TSF 生命周期 | `ctest --test-dir build/windows-tsf/test -C Release -R "^tsf\.lifecycle\." --no-tests=error` | 0 | 3/3 | `build/windows-engine-bridge-tsf-regress-r2.log` |
| P3.2 文本层 | `ctest --test-dir build/windows-text/test -C Release -R "^windows\.text\." --no-tests=error` | 0 | 4/4 | `build/windows-engine-bridge-text-regress-r2.log` |
| P3.3 按键层 | `ctest --test-dir build/windows-input/test -C Release -R "^windows\.input\." --no-tests=error` | 0 | 5/5 | `build/windows-engine-bridge-input-regress-r2.log` |
| 英文正式入口 | `powershell ... tools/test_english_integration.ps1 -EnginePrefix build/librime-english/rime/install -BaselinePrefix build/librime-baseline/rime/install -BuildDir build/english-recovery/test -LogTag p34regress2` | 0 | 29 selected, 0 failed | `RESULT PASS 29 case(s) selected, 0 failed, ctest exit 0` |

## 12. 未验证与明确边界

以下不是“藏在别处”，是本轮**没有**被真实引擎验证的路径：

| 场景 | 状态 | 原因 |
| --- | --- | --- |
| `kDispatchedUnhandled` | **未验证** | P3.3 允许的每个候选都被当前 schema 的 `speller`/`fluid_editor`/space processor 接受；没有一个人工键能在不绕过 Controller 的情况下产生“调用了但返回 False” |
| `kDispatchedStateFailed` | **未验证** | 需要 `get_status`/`get_context`/`get_input` 或 UTF-8 转换失败；真实英文 DLL + 固定夹具没有产生 |
| `kEngineFaulted`（commit 读取失败） | **未验证** | 需要 `get_commit` 返回 True 但无法复制/释放；未构造这种引擎状态 |
| 非 ASCII 文本的候选/commit 副本 | **未验证** | 两个夹具都是 ASCII；UTF-16 转换本身由 P3.2 覆盖 |
| 多字节 preedit/候选的真实映射 | **未验证** | 夹具 preedit 是 ASCII；纯 `state` 模式覆盖了 2/4 字节与所有哨兵 |
| AltGr 文本输入、dead key 组合 | **不支持** | 与 P3.3 一致：AltGr 是 Control+Alt 快捷键；dead key 是边界需求 |
| 错误架构/损坏的 rime.dll | **未验证** | 只测了缺失 DLL、无 `rime_get_api` 的 DLL、无英文模块的 baseline DLL |
| 生产宿主的实际文本写入 | **未实现** | 本轮没有 `ITfRange`、`SetText`、`SendInput`、剪贴板 |
| TSF sink / 注册 / 候选 UI | **未实现** | 本轮不注册、不 Advise、不显示候选 |

“未验证”不等于失败，也不等于通过。它们只能由后续轮次用相应引擎状态或夹具补上。

## 13. 明确不得作出的声明

- 不能声称“实际应用输入完成”：commit 只是待宿主应用的事件，本层没有写入任何应用；
- 不能声称“TSF 按键接入完成”：没有 `ITfKeyEventSink`、`AdviseKeyEventSink`、
  `pfEaten`；
- 不能声称“候选 UI 完成”：没有窗口、没有绘制、没有选中交互；
- 不能声称“多线程安全”：这是 owner-thread 单线程桥接，跨线程调用被拒绝；
- 不能声称“离线/隐私由 OS 强制”：TSF DLL 被任意宿主加载时，离线是架构保证（本层
  无 socket/WinHTTP/WinINet 调用），不是 OS 隔离；Android 的 `INTERNET` 权限缺失
  才是平台强制，两者不能混写；
- 不能声称“所有错误路径都已测试”：见 §12。

## 14. 本轮提交

- 提交 1：`0fc9844a8ace56c98f958a44235991ec7d875db5`
  `feat(windows): add controlled Rime runtime and sessions`
- 提交 2：包含本文件的提交
  `feat(windows): bridge keyboard decisions to engine updates`。Git 提交无法包含自己的
  SHA，完整 SHA 见本轮阶段报告。

两个提交都先构建成功再运行测试；没有把最终状态倒推成中间提交。
