# Engine ABI v1：真实 Rime 适配与会话合同

## 状态与边界

本文件对应阶段 2 的 **ABI、thin adapter 与独立合同验证**，不是产品 TSF 迁移完成声明。

- 公共入口：`engine/include/epocanva_engine.h`，纯 C11 可编译，只依赖 `<stdint.h>`。
- 核心：`engine/core/engine_api.cc`，C++17；版本、句柄、隐私、快照、提交与错误隔离。
- 实际 backend：`engine/adapter/rime_adapter.{h,cc}`，复用已有
  `platforms/windows/engine_bridge/rime_runtime.cc`、`rime_session.cc`、`engine_update.cc`
  以及 `platforms/windows/text/` 的严格转换器。没有第二套 DLL loader、Rime 初始化实现、
  固定候选、空 backend 或 Session 替身。
- Windows/MSVC backend 当前要求已有英文模块。`RuntimeOptions` 采用原有默认值；
  ABI v1 没有扩展成语言包选择协议，也没有重构英文核心或降低其既有行为。
- 产品当前仍经 `RealEngineBackend → InputController → RimeSession`，不是本 ABI。
  Android backend、共享库导出、跨进程 ABI、学习存储写入均不在这次交付内。

旧架构总览中的“未实现”是先前规划快照；本文件仅更新本项的实际状态，不声称其余规划完成。

## 1. 已核对的实现依据

以下依据来自仓库实际源码，不是对上游接口的记忆。上游固定提交为
`33e78140250125871856cdc5b42ddc6a5fcd3cd4`，版本 `1.17.0`。

| 事实 | 依据 |
| --- | --- |
| 一进程只允许一个 runtime 初始化周期 | `platforms/windows/engine_bridge/rime_runtime.cc` 的 `g_runtime_slot_held`、`Start`、`Shutdown`；既有失败隔离/slot 保留逻辑不在此次修改范围 |
| DLL 从明确绝对路径装载并核对实际模块身份 | 同文件的 `LoadLibraryExW`、`LoadedModulePath`、`GetModuleHandleExW` 路径；不链接 `rime.lib` |
| 一个请求只执行一次实际按键/候选操作 | `rime_session.cc:54–103` 的 `ProcessKeyOnce`、`SelectCandidateOnce` |
| 状态查询不消费提交，候选在释放 context 前复制 | `rime_session.cc:105–353`；提交只在 `ReadCommit` 中读取 |
| 原始 preedit 数值为 UTF-8 字节偏移 | `third_party/librime/src/rime_api_impl.h:209–227` 从 `Preedit` 的 `std::string` 复制字节数和字节偏移 |
| 读取成功的提交会推进上游队列 | 同文件 `RimeGetCommit:309–324`，复制后调用 `ResetCommitText` |
| 候选选择用当前页条目序号 | 同文件 `RimeSelectCandidateOnCurrentPage:1045`；不是文本字节偏移 |
| 已有 controller 在实际调用后读取状态和提交 | `input_controller.cc:183–224`、`:265–297`；不能把事后失败表示成“未派发” |
| 字节偏移与 UTF-16 码元偏移不取整 | `platforms/windows/text/text_boundary_map.cc:76–120` 的双向合法边界查询 |
| keysym 修饰位 | `third_party/librime/src/rime/key_table.h:13–45`，掩码 `0x5f001fff` |

`rime_adapter` 只在私有 C++ 层接触这些类型。它将 legacy UTF-16 副本通过已有严格转换器转回
UTF-8，并用 **该份 preedit 的边界表** 将 UTF-16 码元偏移精确映射为 UTF-8 字节偏移。
这多了一次转换，但避免本阶段重写 legacy runtime/session；不是新 Unicode 实现。

## 2. C ABI 布局、错误与所有权

### 2.1 版本

所有可扩展请求/结果结构的前两个字段是：

```c
uint32_t struct_size_bytes;
uint32_t abi_version;
```

调用方先将整结构清零，再设置 `sizeof(struct)` 与 `EPOCANVA_ENGINE_ABI_VERSION`。

- `struct_size_bytes` 以存储字节计。短结构先拒绝，再读后续字段，不能省略版本。
- `abi_version == 0` 或不支持的版本返回 `EPOCANVA_ABI_VERSION_MISMATCH`。
- 大于 v1 已知结构大小但版本仍为 v1：只读/写已知前缀；尾部保持不变。
- `epocanva_context`、UTF-8 view/span 是 v1 内嵌固定布局值；扩展它们需要新主版本。
- 固定宽度整数，不传 `bool`、STL、Win32/Rime 类型或跨 CRT 的分配器对象。
- 使用默认平台对齐，调用方不得用不同 packing；本次实际验证范围为 Windows x64。

`epocanva_status` 是 `int32_t`。成功为 `OK`，没有事件为 `NO_COMMIT`；失败使用
`INVALID_ARGUMENT`、`STRUCT_TOO_SMALL`、`ABI_VERSION_MISMATCH`、`INVALID_UTF8`、
`PRIVACY_DENIED`、`INVALID_HANDLE`、`WRONG_THREAD`、`BUFFER_TOO_SMALL`、
`CONTEXT_MISMATCH`、`GENERATION_MISMATCH`、`STALE_SNAPSHOT`、`OUT_OF_RANGE`、
`BUSY`、`BACKEND_ERROR`、`SESSION_FAULTED`、`OUT_OF_MEMORY`、`INTERNAL_ERROR`、
`BACKEND_VERSION_MISMATCH`。数值以公共头为准。`status_name` 返回静态无用户文本标签，
不回传异常的 `what()`、输入内容或路径诊断。

C++ 异常在 C 入口被捕获：`bad_alloc → OUT_OF_MEMORY`，其他异常 → `INTERNAL_ERROR`。
这不等于能捕获任意无效指针、访问违例、栈损坏或 DLL 内部进程终止。非空指针必须由调用方保证
在整个调用期间可读/写，且请求/结果/文本 buffer 不相互重叠；ABI 不是隔离不可信本机指针的沙箱。

### 2.2 句柄与线程

`engine_handle` 与 `session_handle` 是 `uint64_t` opaque token，不是指针或 Rime session ID。

- 零无效；统一的进程内单调 token 分配器不重用关闭后的值。耗尽时拒绝，不回绕复用。
- engine 在打开它的线程上拥有所有 session。读取、修改、关闭都必须在同一 owner 线程。
- registry 锁保护 token 表并串行执行；它不把 backend 变成可跨线程调用，也不创建 worker。
- engine 有活跃 session 时关闭返回 `BUSY`。先关闭每个 session，再关闭 engine。
- 关闭失败不假装释放 token；已有 runtime 的 terminal finalize/unload failure 语义保留。
- 未显式关闭的对象不会在静态析构阶段从任意线程强制 finalize。
  失败回滚时若 legacy cleanup 抛异常，adapter 保留对象/slot，避免 legacy 析构重试引发终止。
  这是故障隔离下的保留，不是干净释放的承诺。

### 2.3 文本与 caller-owned buffer

所有文本都是严格 UTF-8；每个 `*_utf8_byte_*` 字段以 **UTF-8 代码单元（字节）** 计。
候选序号/页序号计条目/页，不是文本坐标。没有 grapheme cluster 或 caret/screen-cell column
语义。输入标识与路径还拒绝内嵌 NUL，避免 C 字符串截断。

输出是一个 caller-owned buffer，加上若干 `epocanva_utf8_span`：字节偏移相对于 buffer 起点，
字节数不含终止符；**实现不写终止符**。公开文本总量以 `required_utf8_bytes` 返回。

1. `NULL, 0` 查询所需 UTF-8 字节容量。
2. 分配至少该容量，仍传相同 session/context 与（候选的）snapshot version。
3. 成功时复制完整数据；容量不足时填写元数据但不修改任何文本 buffer 字节。
4. 没有 snapshot 指针所有权转移，也没有跨分配器 `free`；调用方自己管理 buffer。

preedit 光标及选区端点是 **相对于 preedit 自身起点** 的 UTF-8 字节偏移，不相对于 raw input，
也不相对于串接 buffer。缺值使用 `EPOCANVA_NO_UTF8_BYTE_OFFSET`；非法边界报错，不 clamp。

## 3. 生命周期与操作语义

### 3.1 打开 runtime

`engine_open(options, &engine)` 将明确的 DLL/共享数据/用户数据/log 绝对路径交给已有
`RimeRuntime`。目录必须已经存在，调用方必须先依据当前 package/build manifest 验证 DLL。
ABI 核对加载后报告的 `expected_rime_version`；**版本匹配不等于 SHA-256 身份验证**。

`deployment_schema_paths` 非空时表示明确的安装器/测试准备请求：沿用 runtime 的一次性部署，
只能是共享目录中的明确 schema 文件。日常运行使用 `NULL/0` 和已编译数据；不在创建 session、
按键或读取时部署，不隐式创建目录、扫描语言包或下载字典。

### 3.2 创建 session 与动态隐私门

`session_open` 必须带非零 `context_target`、非零 `generation` 和 `privacy_class == NORMAL`。
Sensitive、Unknown 及未知枚举值在创建 backend session 前被拒绝。

每次 key、select、snapshot/candidate/commit read、cancel **重新检查请求的 privacy_class**；
不把创建时的 Normal 缓存成永久通行证。拒绝路径不会调用 Rime，也不写入输入文本。

分类由平台适配层根据当前真实上下文取得，ABI 无能力判断调用方是否谎报 Normal。
正常字段进入隐私状态或失焦时，调用方必须停止入队、丢弃旧显示/回调并调用 `session_close`。
close 是无文本提交的清理操作，不要求 Normal 授权。不要以重新标记 Normal 的方式绕过拒绝。

本 ABI 没有学习存储写入；未来接入数据层时，**实际持久化前必须重新判断当时的隐私类别**，
不能根据 session 曾被接纳、候选曾出现或 commit 曾产生而批准写入。已入队的 Normal 请求同样
需要 worker 执行时及实际 host 编辑/持久化时的重新验证；ABI 本身不查询 OS 焦点。

### 3.3 按键与选择

`session_process_key` 接受 X11 keysym 与修饰掩码，不接受 Windows virtual-key 或原始按键日志。
调用方先完成布局/快捷键/AltGr/输入权限策略，不重复进行 Shift/Caps 转换。每次请求至多一次
真实 `ProcessKeyOnce`。成功不代表 handled；请看 `backend_reported_handled`。

候选请求包含 session token、context target、generation、snapshot version、当前页候选序号。
`read_candidate` 和 `select_candidate` 均检查这些值：

- context/generation 不匹配分别拒绝；不能把候选跨字段或跨代投递。
- snapshot version 是进程内唯一 token，每次修改前失效，不能来自另一 session。
- 序号必须在该快照当前页中，不能当成全菜单序号。
- 不匹配时 `backend_call_made == 0`，实际 Rime selection counter 不增加。

每次进入修改前先使旧快照失效并将 session 标为 faulted。操作与事后状态/提交观察全部成功后
才恢复可用。若 backend 已进入但读取/分配/转换失败，返回相应错误，并保留
`backend_call_made == 1`、`state_available == 0`。**调用方禁止重放该请求**，不得当作 host
pass-through 未处理；只能关闭 faulted session，按新的上下文重新建立。

### 3.4 只读快照与提交

修改后捕获状态，并从现有 `ReadCommit` 消费一次 Rime 队列到 ABI 内存缓存。
之后 snapshot/candidate 查询只复制缓存，不触发 process/select/get_context/get_commit，
不会让上次提交混入下一次只读观察。

`read_commit` 仅在一次完整复制成功时消费 ABI 缓存事件。查询容量、容量不足或隐私拒绝均不消费。
无提交返回 `NO_COMMIT`。有 pending commit 时后续 key/select 返回 `BUSY`，必须先复制提交，
或明确 cancel/close；不静默覆盖。snapshot 的 `commit_pending` 是事件可用性提示，成功 read_commit
后可在 snapshot version 不变时归零；backend 文本/候选/偏移内容保持不变。
返回提交只是待应用事件，不表示 TSF/Android 已把它写入宿主。

### 3.5 cancel 与 close

cancel 明确丢弃该会话待提交和组合状态，关闭真实 session，再在同一个 runtime 创建同 schema
的新 session。它不模拟 Escape，不假设所有 schema 对 Escape 的处理相同，也不隐式提交。
成功后 `generation + 1`，返回新 snapshot version；旧 identity 和旧候选失效。
重建失败时 token 留在 faulted 状态，只能 close。

该选择重置 session-local schema options，因此当前合同是“丢弃并重建”，不是无损保留全部
backend session 配置。需要保留未来可变配置时，应另行定义重放/恢复合同，不能悄悄改变 v1。

## 4. 构建接入

### 独立工程

在已有 VS2022/CMake 的环境中（不安装工具）：

```sh
cmake -S tests/engine_abi -B build/engine-abi-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/engine-abi-vs2022 --config Debug --parallel 4
ctest --test-dir build/engine-abi-vs2022 -C Debug --output-on-failure --no-tests=error
cmake --build build/engine-abi-vs2022 --config Release --parallel 4
ctest --test-dir build/engine-abi-vs2022 -C Release --output-on-failure --no-tests=error
```

本机 shell PATH 未找到 `cmake/ninja/cl/clang/gcc/pwsh`；通过已有 CMakeCache 找到已安装 VS CMake
并以绝对可执行路径实跑上面参数。实际工具为 CMake `3.31.6-msvc6`、MSVC `19.44.35209`、
SDK `10.0.26100.0`、Windows x64；C11 `/TC`、C++17 `/permissive-`、`CMAKE_CXX_EXTENSIONS OFF`、
独立测试静态 CRT `/MT`。没有安装、联网、注册、UAC、commit 或产品源码变更。

### 主线已有 bridge 时

产品/root CMake 的拥有者可选择：

```cmake
# Only the reusable API library, after epocanva_engine_bridge is defined:
add_subdirectory("${repo}/engine/core" "${CMAKE_CURRENT_BINARY_DIR}/engine_api")
target_link_libraries(your_consumer PRIVATE epocanva_engine_api)

# Or include the API and its C/real-engine verification suite together:
add_subdirectory("${repo}/tests/engine_abi" "${CMAKE_CURRENT_BINARY_DIR}/engine_abi_tests")
```

两种入口可同时存在：测试工程发现已有 `epocanva_engine_api` 时不重复定义。
子目录模式不调用 `project`，不覆盖主线 CRT；需要时 `enable_language(C)`。
已有 `epocanva_engine_bridge` 时链接该目标，**不能再编译一份 runtime**。
仅独立模式没有 bridge 目标时，才由相同 legacy 源建立 `epocanva_engine_runtime_cpp`。
静态库消费者需通过 CMake target 取得 private 静态依赖的 link closure，而不是只复制一个 `.lib`。

可配置输入：

| 变量 | 意义 |
| --- | --- |
| `EPOCANVA_RIME_DLL` | 当前实际英文 DLL；默认 `build/librime-english/rime/install/lib/rime.dll` |
| `EPOCANVA_RIME_RECEIPT` | 当前 build receipt；默认 `build/librime-english/engine-build-receipt.json` |
| `EPOCANVA_ENGINE_ABI_VERIFY_RECEIPT` | 默认 `ON`，要求 receipt `status=built` 且 DLL SHA-256 与该 receipt 一致；`OFF` 仅作本次 configure 的文件摘要绑定，不证明源码与构建收据一致 |

每次 configure 对 **当前文件** 计算 SHA-256，不固定历史摘要。每个实际引擎测试执行前再次核对
configure-time 摘要；DLL 后来被替换/重建时测试失败，必须重新 configure。
源码 pin 检查实际 `third_party/librime` HEAD；未对整个当前源码 fingerprint 重做 release receipt
审计，此项仍由主线 release gate 负责。

所有测试、schema/dictionary 部署、日志和结果均在 `build/<preset>/`，不修改 fixture 源。

## 5. 实际验证证据

2026-11-02，本独立工程配置、Debug/Release 编译和两种配置的 CTest 均观察到退出码 **0**；
Debug 与 Release 最终各 **9/9**。
真实合同主探针 **299 个检查、0 失败**。首轮 244 个检查之后追加了真实 key 调用后的异常隔离、
faulted 拒重放、取消未读提交和 Unicode commit，不能把首轮数字混作最终数字。

当前已验证 DLL 摘要（仅此轮证据，不是永久配置值）：
`525ceea528cc273b413925b9317bcc550889fff78314f5e7ff4e68875b22a400`，与当时现存 receipt 相符。

| CTest | 探针实际退出码 / 判定 |
| --- | --- |
| `engine_abi.contracts` | 0，`RESULT PASS`；真实英文候选/拼写 comment/提交、多会话、单 runtime、线程、缓冲区、隐私和过期请求 |
| `engine_abi.missing_dll` | 0：断言不存在 DLL 被拒绝，无假 backend 降级 |
| `engine_abi.wrong_rime_version` | 0：断言真实 DLL 的错误 expected version 被拒绝 |
| `engine_abi.abi_can_fail` | **14**，禁止 `RESULT PASS`；故意声称省略 ABI version 会成功 |
| `engine_abi.utf_can_fail` | **14**，禁止成功标记；故意声称 malformed UTF-8 会成功 |
| `engine_abi.privacy_can_fail` | **14**，禁止成功标记；故意声称 Sensitive key 会成功 |
| `engine_abi.stale_candidate_can_fail` | **14**，禁止成功标记；故意声称过期候选会成功 |
| `engine_abi.pure_c` | 0，真正 C11 `/TC` translation unit 编译链接并调用公共 API |
| `engine_abi.pure_c_can_fail` | **14**，C 调用方故意错误断言被精确 exit+marker wrapper 检出 |

Fail controls 使用现有 `tests/english_completion/assert_exit.cmake` 检查 **精确 exit code 与 marker**，
不是简单 `WILL_FAIL`，也不接受测试准备失败作为“成功的负控制”。

真实 Unicode 测试通过 Rime 的 table translator 部署一份小型合同 fixture，生成 `文😀`，证明
preedit/candidate/commit 是 **7 UTF-8 字节**，光标也是 7，而不是 3 UTF-16 码元。
它仅是单位测试数据，不是英文实现替身；英文候选用现有英文 fixture 和实际英文组件。

测试 executable 内的一次性 `operator new` 故障仅触发静态库调用路径，分别抛 `bad_alloc` 和
非标准 C++ 异常。真实 runtime counter 证明修改已执行一次后，结果携带不可重放标记，后续请求
被 `SESSION_FAULTED` 拒绝，最后仍可 close。没有用假的 Rime DLL 来充当真实 backend 验证。

证据文件：`build/engine-abi-vs2022/Testing/Temporary/LastTest.log` 和
`build/engine-abi-vs2022/rime-identity.txt`。主线纳入 add_subdirectory 后的产品级重新构建/验证
由主线执行；本文件不把其尚未执行的结果算入上面 9/9。

## 6. 后续产品 TSF 移植点（本轮未执行）

| 位置 | 需要迁移的合同，不能只换函数名 |
| --- | --- |
| `platforms/windows/engine_executor/real_engine_backend.cc:35–70` | `StartRuntime/CreateClient` 将 runtime owner 与 client 表改成 API engine/session handles；删除旧 `RimeRuntime runtime_` 并停止建立旧 `InputController`，不能旧 runtime 与 API 并行各 Start 一次 |
| 同文件 `Evaluate:73–84` | 保留现有纯 `DecideKey` 对缓存状态的策略，不把 evaluate 改成 `process_key`；ABI 是低层 keysym，不包含 Windows shortcut/AltGr/边界决策 |
| 同文件 `Dispatch:86–97`、`AcceptCandidate:99–109` | 构造实时 privacy/context/generation；将 UI 快照版本一路传到 API，读取结果和待提交事件，显式处理 `backend_call_made` 与 post-call failure |
| 同文件 `Refresh:111–123` | API snapshot 是缓存副本，不偷偷消费 commit；替换 legacy EngineState 读取，并使缓存身份来自 API version |
| 同文件 `CloseClient:125–147`、`CleanupAllClients:149–173` | owner worker 上逐 session close，再 engine close；将失败留在生命周期错误路径，不当作干净 shutdown |
| `platforms/windows/engine_executor/engine_backend.h`、`engine_executor.cc:702–785,922–944` | command/result 当前只携 client/候选序号等，需加可执行时验证的 context/generation/privacy/version；不要仅在入队时存一个过期 Normal 值 |
| `platforms/windows/tsf_coordinator/candidate_snapshot.h:71–107` | UI 当前自建 version、context identity、caret UTF-16 ACP；需保存 API version 或显式建立映射，不能把两个无关版本计数器相互替代。原有 host caret 验证必须保留 |
| `platforms/windows/tsf_coordinator/coordinator.cc` 的 permission/submit/select 路径 | 保留现有真实字段权限和焦点二次验证，隐私变化时清理 session；读取候选/提交以及应用 host edit 之前再确认目标和 generation |
| `platforms/windows/tsf_coordinator/engine_to_plan.cc:42–135` | 将 ABI UTF-8 副本通过 `platforms/windows/text` 转 UTF-16，并针对 **同一 preedit** 建边界表。保留 commit+preedit unsupported-shape 和无实际调用不得编辑的既有决策 |
| `platforms/windows/tsf_product/text_service_controller.cc:542–544` | UI 发出选择时传的是展示该页时的 snapshot version/context/generation；补上 ABI 对应版本，不用“当前”版本替换旧点击 |

后续验收必须覆盖 host caret/focus race、queued privacy change、faulted worker、deactivation/session
retirement、commit 与实际宿主编辑、候选 UI，以及现有英文行为回归。ABI 9/9 不能替代这些测试。

## 7. 安全及未完成项

- API/core/adapter 不新增网络 I/O、遥测、内容日志或学习持久化。DLL/字典来源仍需可信构建与包验证。
- 普通非容器化 Windows TSF DLL 不能由操作系统强制禁止宿主进程网络；这里的离线保证是代码及依赖
  不调用网络接口的架构/构建审查保证。Android 可通过省略 `android.permission.INTERNET` 平台强制
  禁网，但本次没有实现或验证 Android backend，不能将两种保证混为一谈。
- 未实现产品迁移、移动 backend、共享库导出和新英文功能。当前 Windows-specific backend 保留，
  纯 C header 不等于实现已经跨平台。
- legacy session 内部在极端异常时的每个 Rime 分配/释放分支并未全部 fault-inject；C 边界隔离不等于
  第三方或 legacy 任意内存故障均可恢复。已测进入真实 key 后第一次 host-side 分配失败和读取失败隔离。
- CMake 只核对 receipt 中 DLL 摘要与 status，不替主线重算完整 source fingerprint；正式当前收据必须
  在所有相关源码定版后重建并由 release gate 审核。
- `context_target` 是调用方提供的内容无关标识，不是 OS capability。真实焦点/权限/宿主改写责任不可下沉
  成一个可伪造的整数检查；跨 app 一致性仍需产品迁移回归验证。
