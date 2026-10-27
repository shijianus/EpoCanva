# W2-A -- 正式 TSF 产品接线与自然键盘输入

Base HEAD: `fae57a65fe6093880183b5b01eda3a08a16f726c`（与任务书一致）。
本阶段代码提交：`db378c5`（部署一致性）、`4051097`（固定 STA 唤醒）、`9cd82ed`
（key sink 与有界输入准入）。版本/文档/最终验证在同一交付提交中，见 §9 的
“Hand-off HEAD”。

W2-A 是**内部 W2 运行包**，不是最终 Alpha，也没有注册系统输入法。
候选窗口主体留给 W2-B；注册/安装器留给 W3。

## 1. 环境（脱敏）

| 项 | 观测 |
| --- | --- |
| OS | Windows 11 x64，build 26200 |
| 宿主 | Lenovo 83DG 物理机；`HypervisorPresent=False`；**没有**已授权的专用 Windows 测试环境/VM |
| 工具链 | VS 2022 Professional + 内置 CMake 3.31.6 + MSVC 19.44（x64，静态 CRT `/MT`） |
| 引擎 | 既有英文 `rime.dll`，librime 1.17.0（`33e7814`）；本阶段未重建引擎 |
| 数据 | W1 的 42,356 行产品词表与产品 schema/default；W2 只改版本字符串 |
| 注册 | **无**。未写注册表、未注册 CLSID、未切换系统输入法、未使用 regsvr32 |

## 2. 开工最小核查

1. **候选数 2 与编译 schema 1**：W1 报告的 “compiled schema 0/true/1” 是文档数值错误。
   现存同场景产物（`build/product/run/tsf-activation-2`）是 settings `0/true/2`、
   编译 schema `0/true/2`。W2-A 的直接 sink 测试把产品设置设为
   `max_completions=4`、`max_suggestions=2`，并直接断言编译后的 schema 同时包含
   `max_completions: 4` 与 `max_suggestions: 2`（不是 1）。W1 报告该行已更正。
2. **复用 W1 产品路径/配置**：产品 DLL 仍由 `module dir` 解析 data/rime，用户根仍是
   `%LOCALAPPDATA%\EpoCanva`（测试用 `EPOCANVA_USER_DATA_ROOT`），没有新增测试专用配置来源。
3. **产品 DLL 与测试变体**：
   - `epocanva_tsf.dll`：正式产品 DLL，只有两个导出，无测试接缝；key sink 必须
     `AdviseKeyEventSink` 成功，否则激活失败。
   - `epocanva_tsf_direct_sink_test.dll`：同一组产品源码的**受控测试变体**，只多一个
     “未注册时允许无 advice 继续，由测试直接调用 sink”的编译宏，并使用 Lab Win32
     焦点回退；不进入安装包，不导出测试符号。
4. **重复部署**：W1 的“执行器重启后再次部署”已修复。每次激活重新计算指纹；状态匹配则不部署；
   同一进程引擎重启后配置中的一次性 deployment 已被 broker 清除；跨进程由命名互斥量 +
   `engine.lock` 字节范围锁协调。

## 3. 正式生命周期接线

正式产品 DLL 的 `Activate/ActivateEx`（`platforms/windows/tsf/text_service.cc:209`）：

1. `EngineStartTransaction::Prepare` 读取快照、取得跨进程启动锁、安装引擎配置；
2. `EngineHostBroker::Acquire` 获取引擎租约；
3. `Complete` 校验编译产物、写 `deployed-state`、按 token 退役 marker；
4. 构造 `ProductTextController`（`platforms/windows/tsf_product/text_service_controller.cc:137`）：
   `ContextObserver`（**StrictTsf**）→ `ObservedBindingController` →
   `ContextCoordinator`（真实 `PermissionSource` + 既有执行器/编辑层）→ 有界队列；
5. `ITfKeyEventSink` 通过 `ITfKeystrokeMgr::AdviseKeyEventSink` 注册；
6. 提交新状态，释放旧租约/旧控制器。

失败即回滚：新租约、新控制器、新共享锁按相反顺序释放，旧激活保持不变。
`Deactivate`（`:140`）先注销 key sink，再停止控制器（先停 watcher、注销 HWND），
最后释放线程管理器与租约。`DllMain` 仍只记录 HINSTANCE，不初始化、不等待。

产品 DLL 仍只有 `DllCanUnloadNow` / `DllGetClassObject` 两个导出；
`platforms/windows/tsf/epocanva_tsf.def` 未改。

## 4. 固定 STA 结果通知

`StaResultWaker`（`platforms/windows/tsf_product/sta_result_waker.cc:92`）：

- `EngineExecutor` 新增自动复位 `ResultReadyEvent()`；worker 只 `SetEvent`，不访问 COM；
- watcher 线程只 `WaitForMultipleObjects` + `PostMessageW`；消息只有 generation，无文本/借用对象；
- STA 消息窗口核对 HWND 注册与 generation；`Stop` 先停 watcher、再注销、再销毁；
- `PostMessage` 失败计数并 10 ms 重试；结果留在执行器中，唤醒失败不会被当作已应用；
- TSF 异步许可/编辑完成没有可等待的 Win32 事件，watcher 在有未完成工作时 10 ms 节拍唤醒，
  空闲时无限等待；`PumpNow` 幂等，重复唤醒不重复消费。

## 5. 键盘 sink 与输入准入

- `OnTestKeyDown/Up` 只预测：抓取线程键盘状态、`ToUnicodeEx`（bit 2，不改状态）、
  协调器的纯 `Evaluate()`、队列容量；**不调用引擎、不启动许可查询、不请求编辑**。
- `OnKeyDown/Up`：只有真正接受才 `pfEaten=TRUE`；key-up 恒 `FALSE`。
- `KeyAdmissionQueue`（`platforms/windows/tsf_product/key_admission.cc:56`）容量 64；
  无组合时最多 48，剩余保留给组合内输入。每键状态：
  `kNotAccepted / kAccepted / kEngineCompleted / kEditCompleted / kFailed`。
- 同一时刻最多一个在途项；Focus/generation 改变时排队项全部显式失败，在途项保留到
  协调器 `Idle` 才释放，下一键不能越过它。
- Busy/Retiring 由队列吸收，不直接把字符漏进宿主；QueueFull 在无组合时返回 FALSE（宿主保留），
  在组合进行中吃下并记录 `kQueueOverflow` 失败；这是明确报告的残余阻塞（§8），不声称自然输入完整。
- 数字/技术标记按文本候选进入引擎；Ctrl/Alt/Win 快捷键、key-up、未就绪、无可靠上下文、
  无许可/未知许可均按明确策略 pass-through。
- 首键前主动 `ContextCoordinator::RefreshPermission()`（`coordinator.cc:1146`）解析真实许可；
  解析完成前/失败时保持保守 pass-through。

## 6. 部署指纹与跨进程竞争

`EngineStartTransaction`（`platforms/windows/product/product_engine_config.cc:283`）：

- 指纹 = settings token、schema、default、custom patch、product lexicon、rime.dll 的 SHA-256；
- 成功启动后写 `<user>/engine/deployed-state`；**只有记录成功后才**按 token 删除
  `deploy-required`；失败/产物缺失时保留 marker；
- 部署时先取得 `Local\EpoCanva.Deploy.<hash>` 命名互斥量，再对 `<user>/engine/engine.lock`
  取独占字节范围锁；有其它进程的引擎持有共享锁时，本次部署推迟、marker 保留；
- 独占部署期间删除派生的 `<user>/engine/build`，强制从当前输入完整重编译。
  这是修复 rime `ConfigNeedsUpdate` 只比较**整秒**时间戳造成的真实缺陷：
  同一秒内两次设置修改会被判定“未变化”，旧实现会错误记录新快照但保留旧编译值
  （`sink_settings_timing` 已复现并验证修复）；
- 降级为共享锁后持有到该引擎租约结束；运行中的引擎不会被重新部署；
- 设置修改在下一次引擎启动/宿主重启生效，设置界面与诊断文案已按此声明（不再写“W2 下一阶段”）。

## 7. 验证

### 7.1 构建与测试（本机实际命令）

| 命令 | 退出码 / 结果 |
| --- | --- |
| `powershell ... tools/product/build_product.ps1 -SkipTests` | 0 |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error` | 0；135 项，134 通过，1 跳过（`windows.tsf_context_observer.real_focus`，环境限制）；修复窗口类归属后连续两次全量通过 |
| `ctest --test-dir build/vs2022-x64-debug -C Debug ...` | 0；8/8 |
| 独立 P3.1 TSF 工程 `build/windows-tsf/test` 构建 + CTest | 0；3/3 |
| `dumpbin -exports epocanva_tsf.dll` | 恰好 `DllCanUnloadNow`、`DllGetClassObject`；无 `EpoCanvaTest*` |
| `dumpbin -imports` 三个产物 | 未出现 `WS2_32/WININET/WINHTTP/URLMON/DNSAPI/WSOCK/SECUR32/SCHANNEL`（仅静态导入表证据） |
| `stage_product.ps1 -Replace` | 0；`EpoCanva-0.2.0-w2`，21 文件，4,184,832 字节 |
| 暂存包 `EpoCanva.exe --diagnose` | 0；`package=ready`，21/21 文件校验通过 |

新增/相关 CTest：

- `product.integration.deployment_snapshot`：指纹生命周期、marker 顺序、**真实跨进程竞争**；
- `product.unit.sta_waker`：唤醒、generation、迟到消息、停用注销、PostMessage 失败计数、节拍；
- `product.unit.key_admission`：容量、五状态、FIFO、在途失败、显式溢出；
- `product.integration.sink_raw`：真实产品源码 + 受控 TextStore + 直接 sink 调用；
- `product.integration.sink_engine_missing`：正式产品 DLL、缺 `rime.dll` 时激活失败且 marker 保留；
- `product.integration.sink_multi`：同进程两个 STA 会话共享一个引擎，各自输入；
- `product.integration.sink_settings_timing`：运行中改设置 → marker 保留、旧 engine 不重部署 →
  全部停止后新 engine 编译新设置；
- `product.integration.sink_focus_switch`：子上下文 Push 后旧文档不再被写、新上下文可输入；
- `product.integration.sink_settings_timing` 同时覆盖“运行中改设置不热更新、停止后部署新值”；
- `product.integration.sink_strict_unregistered`：正式产品 DLL 在“无法 advise key sink”的
  未注册环境**拒绝激活**，且不吞键、不改文档。

### 7.2 直接 sink 覆盖（固定数据，真实英文引擎）

覆盖并通过：大小写保真（`Hello`）、默认原文不自动纠错（`helo`）、空格、Enter（组合内吃掉，
不丢组合，随后空格提交）、BackSpace、Escape、数字与技术标记、邮箱、URL、Windows 路径
（含反斜杠/冒号）、标识符、Ctrl+C/V/Z、Alt+X、Win+R、连续快速输入、外部客户端写入后的
代际切换与重新绑定、子上下文 Push 的焦点切换（旧文档不再被写）、只读/密码 InputScope/
未知许可 pass-through、中文用户目录（`<run-root>/用户 数据`）。

### 7.3 真实应用输入

**未验证，且本机不允许绕过。** 没有已授权的专用 Windows 测试环境；任务书禁止在用户主系统
注册或切换输入法。未注册 TIP 的 `AdviseKeyEventSink` 在本机返回 `E_INVALIDARG`
（合成 TfClientId 不能注册真实 key sink），因此直接 sink 测试使用同源测试变体；
正式产品 DLL 在同场景正确拒绝激活。真实注册应用输入留给有授权 VM 的验证。

### 7.4 一次真实崩溃与修复

全量套件第一次运行 `sink_raw` 时出现过一次 `0xc000041d`（用户回调异常）。定位为
`StaResultWaker` 用宿主 EXE 的 HINSTANCE 注册窗口类、WndProc 却在产品 DLL 内：DLL 卸载后
类仍指向已卸载代码，下一次 `CreateWindowExW` 即崩溃。修复为使用 DLL 自身 `ModuleInstance()`
注册、引用计数并在最后一个 waker 停止时 `UnregisterClassW`。修复后 `sink_raw` 单独重复 3 次、
全量套件连续 2 次均通过。该问题说明窗口类/模块卸载路径必须由产品 DLL 拥有，不能借用宿主 EXE。

## 8. 未完成、阻塞与风险

1. **候选 UI**：W2-B；本轮不显示候选、不分页、不提供数字选择。
2. **队列完全耗尽且组合进行中**没有安全 pass-through；实现吃键 + 显式失败，属于
   **未解决的容量耗尽策略**，自然输入在极端引擎停顿下不宣称完整。
3. **首键许可解析**：绑定后主动解析真实许可；解析完成前的按键保守 pass-through，
   因此聚焦后第一键可能由宿主直接插入。敏感/未知许可始终 pass-through。
4. **设置生效时机**：下一次引擎启动或宿主重启；没有跨宿主即时热更新。
5. **真实应用/注册**：本机无授权隔离环境，未注册；真实应用输入、注册/卸载、安装器
   必须留给 W3 的 VM 矩阵。
6. **StrictTsf**：产品策略保持 StrictTsf；本机无真实前台焦点，`real_focus` 跳过，
   受控直接 sink 测试使用 Lab Win32 fallback。StrictTsf 的真实前台路径未在本机执行。
7. **跨进程运行期共享锁**：跨进程部署竞争已实测；两个真实应用同时运行引擎的共享锁路径
   未做双进程长时输入测试。
8. **rime.dll 内嵌构建路径**：仍为 68 处，公开分发前必须用 `/pathmap` 固定重建。
9. **词表再分发**：仍待 owner/legal 决定；本包仅内部。
10. **DPI**：150% 已验；其他比例/小工作区仍是 W1 的未覆盖项。

## 9. 变更文件与交付

新增：

- `platforms/windows/tsf_product/key_admission.{h,cc}`
- `platforms/windows/tsf_product/sta_result_waker.{h,cc}`
- `platforms/windows/tsf_product/text_service_controller.{h,cc}`
- `platforms/windows/tsf_product/tests/key_admission_tests.cc`
- `platforms/windows/tsf_product/tests/sta_waker_tests.cc`
- `tests/windows/tsf_product/product_sink_tests.cc`
- `platforms/windows/product/tests/deployment_tests.cc`
- `docs/windows/tsf-keyboard-input-contract.md`
- `docs/reports/W2-A-product-input.md`

修改：

- `platforms/windows/tsf/text_service.{h,cc}`：生命周期、key sink、部署事务、租约锁
- `platforms/windows/tsf/engine_host.{h,cc}`：租约暴露执行器、一次性部署、运行配置比较
- `platforms/windows/engine_executor/engine_executor.{h,cc}`：`ResultReadyEvent`
- `platforms/windows/tsf_coordinator/coordinator.{h,cc}`：`RefreshPermission`
- `platforms/windows/tsf_product/...`、`platforms/windows/product/...`：见提交
- `data/product/default.yaml`、`data/product/epocanva_english.schema.yaml`、
  `platforms/windows/product/VERSION`：`0.2.0-w2`
- `platforms/windows/product/product_diagnostics.cc`、`settings_window.cc`：能力/注册时机文案
- `README.md`、`docs/reports/W1-windows-alpha-product.md`：状态与候选数更正

运行包：`build/product/stage/EpoCanva-0.2.0-w2/`（21 个文件；`package.manifest` 的 commit
在本交付提交后重建时会指向 Hand-off HEAD）。重建入口：
`tools/product/build_product.ps1` + `tools/product/stage_product.ps1`。

Hand-off HEAD：见本次会话最终 `git log -1`；本文档随最终提交一起交付，
代码提交为 `db378c5`、`4051097`、`9cd82ed`。
