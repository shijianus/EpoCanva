# W2-A-R -- 输入可靠性收敛

起点 HEAD：`626061af8351a51fb2e90eaccb05337925335ebf`（W2-A 的交接提交）。
本轮没有重做 W2-A，也没有进入候选窗口或安装器。W2-A-R 仍是**内部运行包**，
不是最终 Alpha，也没有注册系统输入法。

## 1. 环境（脱敏）

| 项 | 观测 |
| --- | --- |
| OS | Windows 11 x64，build 26200（26200.9711） |
| 宿主 | Lenovo 83DG 物理机；`HypervisorPresent=False`；**没有**已授权的专用 Windows 测试环境/VM |
| 工具链 | VS 2022 Professional + 内置 CMake 3.31.6 + MSVC 19.44（x64，静态 CRT `/MT`） |
| 引擎/数据 | 既有英文 `rime.dll`（librime `33e7814`）、W1 产品词表与 schema；本轮未重建引擎 |
| 注册 | **无**。未写注册表、未注册 CLSID、未切换输入法、未使用 regsvr32 |

隔离环境为只读核查：没有授权环境，因此没有注册后的正式 DLL、StrictTsf 真实前台或真实应用
输入证据。直接 sink + Lab Win32 fallback 仍只算**受控测试**，不补成正式注册环境通过。

## 2. 三个问题的定位与修复

### A. 组合中队列耗尽后吃键并记录失败（以及更早的乱序）

真实源码（起点状态）：

- `platforms/windows/tsf_product/key_admission.cc:56` `CanAccept`：无组合 48、总容量 64；
- `key_admission.cc:247` `RecordOverflowEaten`：吃键并只记失败；
- `text_service_controller.cc:307`（原 `HandleKeyDown`）：队列满且**当前快照没有组合**时
  直接 `return false`。这是一个真实乱序缺陷：队列里已有“已接受、未应用”的键，宿主立即插入
  新字符会排在这些键之前；随后协调器再把旧键写成 preedit/commit，文档顺序就被打乱。
- `text_service_controller.cc:313`（原）：组合内满时 `RecordOverflowEaten` + 吞键。

修复（不靠增大容量）：

1. `KeyAdmissionQueue::kHighWater = 24`（低于两个硬上限）。达到高水位时，控制器把
   队列中**尚未提交**的尾部按 FIFO 撤出（`WithdrawQueued`），折算成原样 UTF-16 文本；
   在途的单个引擎键不动。撤出的键记为 `deferred_raw`，不是失败。
2. 降级期间的文本键与 Space/Enter/Escape/BackSpace 继续被吃下并折叠进这段文本。
   超过 `kRawCapacityUtf16 = 1024` 时键仍被吃下以保住已有顺序，但记
   `raw_dropped_keys` / `failed_raw_capacity`，**不声称已应用**。
3. 协调器新增 `SubmitRawCommit`（`coordinator.cc`）：空闲时把这段文本一次写入文档，
   **不调用引擎**。若写者仍拥有组合，plan 用“当前显示文本 + 捕获后缀”替换并结束组合；
   否则在跟踪的 caret 处插入。原文优先于补全与拼写建议。
4. 提交成功后引擎客户端已过期：控制器使绑定退役并重建，之后恢复引擎路径；不做超时重放。
5. 任何无法提交的降级段以 `raw-commit-failed` / `raw-capacity` / `kPermissionRefused`
   明确结束，不保留“吃下只记错”的路径。

分账（`KeyAdmissionSnapshot`）：`accepted → dispatched → engine_completed →
edit_completed` 是引擎路径；`deferred_raw → raw_applied` 是降级路径；
`raw_failed / raw_dropped / failed_*` 是所有明确失败。控制器诊断另有
`raw_unapplied_units`（长度，不含文本）与 `engine_stale`。

### B. 许可就绪前首键直通，后续可能中途接管

真实源码：起点控制器没有任何 token 归属状态。权限 Unknown 时 `DecideKey` 令首键直通；
若许可在同一个词的两个字符之间解析为 Allowed，下一字符就会进入引擎组合，文档被拆成
“宿主已插入的前缀 + 引擎 preedit 的后缀”。焦点丢失/外部编辑还会让绑定重建，进一步
放大这个窗口。

修复：

- `HostTokenOwnership`（`key_admission.h`）：一个 token 的首字符交给宿主后，余下字符一律
  直通，直到空格、非 token 标点、编辑键或焦点真正移走。
- token 字符 = 英文 schema 字母数字 + `' - @ . _ : / \ ? & = # % +`；**不把 @、路径、
  URL 标记当普通分词边界**。快捷键不结束 token。
- 控制器在 `OnTargetLost(kExternalTextEdit/SelectionEdit)` 与 `OnTargetBound` 后**保留**
  token 归属：宿主插入正是这些外部编辑的来源；只有焦点真正移走或 Stop 才清除。
- Unknown/Denied 永不向引擎发送输入（既有 `DecideKey`/`SubmitKey` 许可门保持）。
- 不读取整篇文本；只使用既有的 selection/InputScope 元数据读取。不补造未捕获的输入。

### C. 运行期共享锁与部署独占锁

真实源码：起点 `text_service.cc` 从未调用 `EngineStartTransaction::TakeLeaseLock()`。
`EngineStartTransaction::Complete` 把独占锁降级为共享锁，但事务对象在 `ActivateInternal`
结束时析构并关闭句柄——**运行中的引擎实际不持有共享锁**。因此另一个进程只要认为需要部署，
就能取得独占锁、删除 `engine/build`，而这里还有活引擎在使用它。头文件声称的
“每个活动引擎持有共享锁”与代码不符。

修复（`text_service.cc`）：

- `Complete` 成功后立即 `new_lease_lock = deployment.TakeLeaseLock()`；
  共享锁句柄由激活状态持有到 `Deactivate`（先释放引擎租约/停止执行器，再关闭句柄）。
- 控制器启动失败、激活失败、异常路径都关闭该句柄；异常析构也关闭遗留句柄。
- 部署等待保持有界：另一进程持共享锁时，独占尝试失败即**降级为共享执行**（旧配置可用、
  marker 保留、不删 build），到下一次真正引擎启动再部署；命名互斥量 + 先独占后共享的
  单一方向避免了共享锁升级死锁。激活中的等待上限仍是 60s（有界，不是无限）。

## 3. 测试宿主的默认输入与独立参考模型

`tests/windows/tsf_product/product_sink_tests.cc`：

- `OnTestKeyDown` 每次调用两次并比较：**可重复、无副作用**；
- `OnKeyDown` 必须与预测一致；`pfEaten=FALSE` 时由测试宿主用真实的外部 TSF 编辑会话
  插入该字符/Space，**不再跳过这个键**；
- `SendTextAdaptive` 记录每个键的真实 eaten 状态；
- `ReferenceDoc` 是独立的手写编辑模型（只由固定测试数据和文档化的默认英文行为生成），
  不调用任何产品代码。

覆盖：连续快打、重复字母、大小写、Space/Enter/BackSpace/Escape、邮箱/URL/路径/标识符、
异步许可、许可中途变 Allowed、延迟编辑（文档锁被测试持有）、高水位/降级、降级期间停用、
焦点切换、外部编辑后重绑、只读/密码/未知许可。生产日志不保存输入文本；测试只用固定数据。

## 4. 实际验证

### 4.1 构建与测试（本机实际命令）

| 命令 | 退出码 / 结果 |
| --- | --- |
| `powershell -File tools/product/build_product.ps1 -SkipTests` | 0 |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error` | 0；138 项：**137 通过，0 失败，1 跳过**（`windows.tsf_context_observer.real_focus`，无真实前台焦点） |
| `cmake --build build/vs2022-x64-debug --config Debug` | 0 |
| `ctest --test-dir build/vs2022-x64-debug -C Debug ...` | 0；8/8 |
| 独立 P3.1 TSF 工程 `build/windows-tsf/test` 构建 + CTest | 0；3/3 |
| `dumpbin /exports epocanva_tsf.dll` | 恰好 `DllCanUnloadNow`、`DllGetClassObject` |
| `dumpbin /imports epocanva_tsf.dll` | 无 `WS2_32/WININET/WINHTTP/URLMON/DNSAPI/WSOCK/SECUR32/SCHANNEL` |
| `stage_product.ps1` + `EpoCanva.exe --diagnose` | 见 §6 |

新增/扩展 CTest：

- `product.unit.key_admission`：高水位、FIFO 撤出、raw 分账、首 token 规则（纯类型单测）；
- `product.integration.sink_token`：未知许可首字符直通 → 字段变为 Allowed → 同一 token
  余下字符仍直通 → 边界后新 token 由引擎提交；最终文档等于参考模型；
- `product.integration.sink_pressure`：文档锁被持有背景下的 65 字符突发 + 空格、
  释放后精确一次应用；300ms 后无迟到重复；引擎重建后下一 token 正常；第二阶段在
  “引擎结果被消费、编辑被拖延”的情况下持续输入 33 字符 + 空格；最后在降级待应用时停用；
- `product.integration.sink_lease_lock`：产品 DLL 活引擎期间，另一个进程的部署**推迟**，
  不删编译产物、marker 保留、记录不更新；停用后同一部署成功；
- `product.integration.deployment_snapshot` 新增 runtime holder：新宿主在共享锁下不被阻塞
  （<5s）；改动设置后的部署推迟；holder 退出后部署生效并重建 build。

压力结果（固定数据，只有长度与相等性断言）：

- 65 字符突发 + 空格：最终文档与 `ReferenceDoc` **完全相等**，无丢失、无重复、无乱序；
  之后 300ms 泵消息文档不变；
- 引擎重建后追加 `tail `：顺序正确；
- 33 字符持续输入 + 空格（编辑被拖延）：最终文档与参考模型完全相等；
- 降级待应用时 `Deactivate` 返回，进程完成清理（无挂起：CTest 超时未触发）。

### 4.2 部署等待（应用 A 旧配置 → 修改设置 → 应用 B）

- 应用 A 的引擎租约期间持有共享锁；B 的部署获取独占失败 → 明确推迟：B 仍以旧配置激活，
  marker 保留，`build` 与其 sentinel 均保留，`deployed-state` 不写入新指纹；
- A 退出后，下一次真正启动删除旧 build 并写入新指纹，marker 按 token 退役；
- 新宿主（无需部署）在 A 运行时正常激活，未等待 60s 部署锁；
- 没有共享→独占升级路径，因此没有升级死锁；激活等待是有界 60s。

## 5. 提交

按真实修复范围小步提交（完整 SHA 见交付消息与 `git log`）：

1. 协调器原样降级提交 + 准入队列/首 token 规则 + 控制器降级路径；
2. 运行期共享锁持有到租约结束；
3. 测试宿主默认输入、参考模型、压力/首 token/租约/部署测试；
4. 契约文档、README、版本与报告。

没有空提交、没有 amend/reset --hard/clean、没有 push/tag。

## 6. 运行包

暂存包：`build/product/stage/EpoCanva-0.2.1-w2r/`。构建入口：
`tools/product/build_product.ps1` + `tools/product/stage_product.ps1 -Replace`。
包内 `VERSION.txt` 为 `0.2.1-w2r`；`package.manifest` 的 commit 指向本轮交付 HEAD。
`EpoCanva.exe --diagnose` 的 21/21 文件校验结果见交付消息（实际退出码，不预填）。

## 7. 未完成、风险与下一阶段门

1. **无已授权隔离环境**：未注册、未验证正式 DLL 在真实应用/StrictTsf 真实前台下的输入；
   直接 sink + Lab fallback 只是受控测试。注册/卸载/安装器留给 W3 的 VM 矩阵。
2. **候选 UI**：W2-B；本轮不显示候选、不分页、不提供数字选择。
3. **降级期编辑键语义是明确折衷**：Enter 吃下但无文本（与现行为一致）；Escape 结束降级段
   但**不取消**已捕获文本；BackSpace 只回退捕获后缀，目标落在已显示文本时记为
   `raw_ignored_edits`。这些都在契约中声明，不声称与正常引擎路径完全等价。
4. **降级运行不自动恢复补全**：恢复发生在 token 边界 + 缓冲清空 + 引擎重绑之后；没有定时器
   触发的“超时恢复/重放”。
5. **不可恢复上限**：捕获超过 1024 个 UTF-16 code unit 时，后续键仍被吃下以保住顺序，但
   明确记为 `raw-capacity` 丢失；产品 DLL 目前只有内部计分，没有用户可见诊断面板。
6. **真实延迟引擎**没有注入接缝：压力测试用真实引擎 + 测试持有的文档锁/不泵消息制造
   高水位；协调器级“引擎结果未取”的路径由不泵消息覆盖，不是人为延迟 worker。
7. **跨进程产品租约**由“产品 DLL 活引擎 + 部署子进程”验证，不是两个完整注册宿主。
8. **激活等待有界 60s**：长部署期间新宿主的 UI 线程会等待该上限；未改成异步激活。
9. `rime.dll` 内嵌构建路径仍为 68 处；公开分发前必须 `/pathmap` 重建；词表再分发仍待
   owner/legal 决定；DPI 仅 150% 已验。
10. 设置修改仍在下一次引擎启动/宿主重启生效，没有跨宿主热更新（沿用 W2-A 声明）。
