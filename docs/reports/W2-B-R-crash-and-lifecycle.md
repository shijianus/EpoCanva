# W2-B-R -- 容量边界后的同会话恢复、控制键端到端与卸载生命周期

起点 HEAD：`aaa772c74f5480f8e5d6506a6a7a8f24ec4157e0`（W2-B 文档提交）。
W2-B 源码提交：`c884e3aec92a4a61a963465ee0953df3f5aafd9b`。
本轮源码/测试提交：`5a5edd2bd36a029341b5dfff4da98ae5bf6a58e9`。
包版本提交（实际构建身份）：`08d177caceaaa544b6ce00774600b63fa5fbee3b`。
本报告在文档提交中；没有 push/tag/amend/reset --hard/clean。
仍为**内部运行包**：未注册、未切换系统输入法、没有已授权隔离环境。

## 1. 环境（脱敏）与隔离确认

| 项 | 观测 |
| --- | --- |
| OS | Windows 11 x64 build 26200（26200.9711） |
| 故障模块 | `textinputframework.dll` 10.0.26100.9711；`msctf.dll` 10.0.26100.8875 |
| 工具链 | VS 2022 Professional + 内置 CMake 3.31.6 + MSVC 19.44（x64，静态 CRT `/MT`） |
| 引擎/数据 | 既有英文 `rime.dll`（librime `33e7814`）、W1 词表与 schema；未重建引擎 |
| 注册 | **无**。未写注册表、未注册 CLSID、未切换输入法 |
| 构建/运行位置 | `build/windows-product/`（OneDrive 同步卷，I/O 抖动风险仍在） |

只读核查：没有已授权的专用 Windows 环境（物理机，`HypervisorPresent=False`），因此没有
C 层（正式注册 + StrictTsf + 真实应用）。隔离方案与一次性授权请求见 §10。

## 2. 原始复现与故障证据

### 2.1 已有的 Windows 错误报告（WER）

只读查询 Application 日志，找到 **4 次同一偏移**的崩溃，全部发生在 2026-10-27 的
Release 产物 `build/windows-product/product/bin/epocanva_product_sink_tests.exe` 上：

| 时间 | 应用时间戳 | 报告 Id |
| --- | --- | --- |
| 2026-10-27 23:38:18 | `6ae0c54a` | `006f6a8f-094c-4b9f-b8b3-66b2719e162c` |
| 2026-10-27 23:39:32 | `6ae0c594` | `430226e6-54e3-4bfa-b908-7eb9d7dbca19` |
| 2026-10-27 23:41:43 | `6ae0c60b` | `ae301eef-0883-4e6b-81d7-05fd083eebdf` |
| 2026-10-27 23:42:57 | `6ae0c661` | `641f7f30-a0d2-4268-9b1f-93e5a7b44743` |

共同事实（WER `Report.wer`，位于
`C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_epocanva_product_*`）：

- 异常码 `0xC0000005`，故障模块 `textinputframework.dll`，版本 `10.0.26100.9711`，
  **偏移 `0x270a5`**；
- 故障应用是受控 sink 测试程序，路径在 `build/windows-product/product/bin/`；
- 报告中的模块列表只含系统模块与测试 EXE，没有产品 DLL/`rime.dll`（该列表可能被截断，
  不能单独作为“已卸载”的证明，见 2.2）。

### 2.2 偏移所在代码的只读反汇编

用 PE 头把 `C:\Windows\System32\textinputframework.dll` 的 `.text` 窗口映射到
`file offset 0x26000`，`dumpbin /disasm` 显示 RVA `0x270a5` 位于一个清理/析构函数中：

```
180002099: mov rcx,[rcx+40h]      ; a member interface
1800020A0: je  ...
1800020A2: mov rax,[rcx]          ; vtable pointer of the member
1800020A5: mov rax,[rax+10h]      ; Release slot
1800020A9: call ...               ; invoke Release
```

故障在 `mov rax,[rax+10h]`：成员对象的 **vtable 指针本身不可读**。这与“对象已被释放/
其 vtable 位于已卸载镜像”一致，而不是 TSF 内部的普通空指针。随后该函数继续对
`+0x28/+0x30/+0x38` 的成员做同样的 Release。

### 2.3 本机复现：卸载契约被违反时，执行位于已卸载镜像

修复前的受控宿主在 `Deactivate` 后**先 `FreeLibrary`，再拆除 TSF 宿主**，且不检查
`DllCanUnloadNow`。把这一顺序用于“仍有 TSF 持有的编辑会话”的探针，得到：

- 首机会话异常：`0xC0000005`，**execute at `0x7FF95EB41120`**，该地址不在任何已加载模块中；
- minidump 与首机会话文本（仅开发测试目录）：
  `build/windows-product/product/run/capacity-v9/diagnostics/crash-20261029-001808-537.dmp`、
  `.../first-chance-1-...txt`；
- dump 的模块列表中没有 `epocanva_tsf.dll`/`rime.dll`，崩溃发生在宿主拆除阶段。

同一轮还定位到一个测试宿主的活性缺陷：`PumpUntil` 的 500 ms 超时对每次
`MsgWaitForMultipleObjects` 重新计时，而产品在等待写授权时每 10 ms 唤醒一次，导致
切片永不返回。看门狗转储
`build/windows-product/product/run/capacity-v10e/diagnostics/hang-20261029-004410-880.dmp`
记录了主线程停在 `TestHost::PumpUntil` 的 `MsgWaitForMultipleObjectsEx`。

### 2.4 恢复的“同会话继续输入”用例

`product.integration.sink_capacity_continue`（新增）严格按原始场景执行：真实异步文档锁被
持有 → 首个引擎键在途 → 发送 `ab` + 1200 个 `x` → 跨越高水位、填满 1024 单元原文块、
越界键记为丢弃 → 释放锁并应用整块 → **同一激活、同一上下文立刻**输入 `after` → Space
提交 → 引擎路径在同一会话恢复 → `hel` + 方向键 + Enter 提交补全 → 停用并清理。

**诚实结论**：在最终代码上该用例在 Release 与 RelWithDebInfo 上通过；在修复前，本机
**没有**复现出“继续输入时”的同一个 `0x270a5` 硬故障。复现到的是同一根因链的**卸载期
访问冲突**（2.3）。原始崩溃的精确测试版本没有提交到 git，无法逐字恢复；WER 的故障指令
（对成员取 vtable 后调用 Release）与卸载期对象生命周期一致，因此根因判定为卸载/引用
生命周期，而不是输入路径中的 TSF 缺陷。

## 3. 根因与代码修复

### 3.1 异步编辑会话泄漏一个引用（产品缺陷）

`platforms/windows/tsf_edit/edit_session.cc` 的 `Submit` 在 `phr_session == TF_S_ASYNC`
时保留了构造引用，又对 `pending_callback` 额外 `AddRef`；`PendingResult`/`Close` 只释放
一次。结果是**每个完成的异步编辑泄漏一个 `ITfEditSession`**（容量继续输入场景实测
`edit_session=9` 个存活对象）。TSF 持有这些回调；模块因此永远不可卸载，且一旦被强制
卸载，TSF 之后释放它们就会调用已解除映射的 vtable。

修复：把构造引用**转移**给 `pending_callback`，不再额外 AddRef；由
`PendingResult`/`Close`/`State` 析构各释放一次。

### 3.2 `DllCanUnloadNow` 没有统计 TSF 持有的回调

`module_lifetime.h`（header-only，按镜像计数）由 `ProductTextController::Start` 安装
`ModuleAddObject/ModuleReleaseObject`。以下对象在存活期间计入卸载判断：
组合编辑 `ITfEditSession`、选择读取 `ITfEditSession`、`ITfCompositionSink`、
候选矩形读取 `ITfEditSession`、`ITfCandidateListUIElementBehavior`、`ITfUIElementSink`。

`DllCanUnloadNow` 仍然是四个计数器的只读判断，不 join、不加锁、不触碰 TSF。

### 3.3 失败的激活留下无人接管的引擎启动

`text_service.cc` 原先在 `AdviseKeyEventSink` **之前**就通过控制器请求后台引擎启动；
严格产品 DLL 未注册时 Advise 被拒绝，回滚只停止控制器，不能取消已经发出的启动。启动
完成后引擎运行而 `host_active` 永远为 1，模块不可卸载，进程退出时后台执行器线程仍在
DLL 内 → 访问冲突。

修复：`ProductTextController::Start` 只建立 STA 控制器/唤醒通道；
`RequestEngineStart()` 仅在 `AdviseKeyEventSink` 成功之后调用。激活仍是异步的。

### 3.4 测试宿主违反卸载契约

`tests/windows/tsf_product` 的顺序改为：`Deactivate` → 释放 TIP/工厂 → **拆除 TSF 宿主**
（Pop/CoUninitialize，让 TSF 释放回调）→ 以 10 s 为上限等待 `DllCanUnloadNow == S_OK`
→ 只有 `S_OK` 才 `FreeLibrary`。`S_FALSE` 时保留模块并打印
`[unload] ... stayed S_FALSE ... (not forced)`，绝不强制卸载。并发会话的 LoadLibrary
引用在全部会话结束后统一释放（`DeferProductModule` / `FreeDeferredProductModules`）。

同时：`PumpUntil` 使用**整体截止时间**并在每个切片最多派发 64 条消息；新增
`DropHeldGrantMessages` 用于“应用始终不授予写锁”的焦点切换场景。

### 3.5 原子替换在引擎关闭旧文件时的短暂共享冲突

全量回归中出现 `replacing ...\epocanva_english.custom.yaml failed (error 5)`（15 次中
1 次）。`WriteFileAtomically` 现在只对 `ERROR_ACCESS_DENIED`/`ERROR_SHARING_VIOLATION`
做 20×50 ms 的**有界重试**，其他错误仍然立即失败；写入内容与语义不变。单测用真实
“拒绝删除共享”的读者覆盖该窗口；`sink_settings_timing` 15/15 通过。

## 4. 为什么修复能解释原故障

1. 容量继续输入会连续产生大量异步编辑；旧代码每个编辑泄漏一个回调，TSF 持有它们；
2. `DllCanUnloadNow` 只统计工厂/TIP/锁/引擎窗口，不统计这些回调，因此在仍有 TSF
   引用时返回 `S_OK`；
3. 受控宿主不检查该回答，先 `FreeLibrary`，后 Pop/CoUninitialize；DLL 镜像解除映射；
4. TSF 在拆除上下文时释放它仍持有的回调/sink，读取已解除映射的 vtable → 在
   `textinputframework` 的清理函数中以 `0xC0000005` 结束（与 WER `0x270a5` 的
   `mov rax,[rax+10h]` 形态一致）。

因此“继续输入新 token”不是输入路径本身触发 AV，而是它制造了更多**在卸载时仍未释放**
的会话；删除继续输入改变了时序，症状被隐藏，问题没有解决。修复同时消除了泄漏、让
卸载门真实拒绝、并让宿主按契约拆除；三者缺一不可。

## 5. 容量边界事件归属（固定输入）

`product.integration.sink_capacity_continue`，1202 个 UTF-16 单元（`ab` + 1200×`x`），
真实异步文档锁：

| 归属 | 实测 |
| --- | --- |
| 已接受 | 1202/1202 `pfEaten=TRUE`（`eaten == key_downs`） |
| 已应用 | `raw_applied_units == 1024`；文档为键入前缀，长度 1024/1025（含首引擎键） |
| 明确丢弃 | `raw_dropped_keys == 177`；`retained + dropped == typed` |
| 用户取消 | 0 |
| 仍待恢复 | 队列/在途为 0；之后同一会话引擎菜单/选择恢复 |
| 重复/迟到 | 块只写一次；300 ms 后文档不变 |

继续输入的 `after `、恢复后的 `hel`+补全选择分别单独断言；用例结束断言模块已卸载。
代理项对的原子追加/回退由 `product.unit.key_admission` 覆盖；B 层键盘路径没有合法的
astral 输入，未用合成事件冒充。

### 5.1 W2-C 更正（2026-10-29，后续轮次追加）

上表是 W2-B-R 当时的**原始读数**，其中两处必须更正；行为本身没有被文字掩盖：

1. “已应用 1024”只统计 `raw_applied_units`，漏掉了首个引擎键 `a`。实际文档是
   **1025** 个 UTF-16 单元（1 个引擎键 + 1024 个 raw 块单元）。
2. “明确丢弃 177”不是用户取消，也不是宿主默认处理，而是**内部容量丢失**：
   键 1026–1202（177 个 `x`）在 `pfEaten=TRUE` 之后因 raw 块满且
   `FlushRaw()` 被在途的首个引擎键阻塞而 `RecordRawDropped`。宿主从未收到这些键。

更正后的完整归属：输入事件 1202（key-down）、文本单元 1202、已接受
`pfEaten=TRUE` 1202、宿主默认处理 **0**、已应用 **1025**、结算后待应用 0、
用户明确取消 0、**内部丢失 177**。`retained + dropped == typed` 只是会计恒等式；
它由产品自身状态构造（`retained` 直接取自产品文档长度），所以旧用例把丢失当成了
合法期望。W2-C 删除 raw 路径并改用独立参考模型后，同样输入得到 1202/1202 宿主插入、
0 内部丢失、文档等于完整参考（见 `W2-C-input-fidelity.md`）。

## 6. 降级期控制键端到端

`product.integration.sink_degraded_keys`（真实控制器、协调器、TSF 编辑会话）：

- **BackSpace**：真实 backlog 下的未提交后缀回退一个 code point，文档与预期完全相等，
  `raw_ignored_edits == 0`；
- **Enter**：结束降级段并提交后缀，不插入换行；先前已提交块保留；
- **Escape**：只取消未提交后缀；已交给协调器的首块仍按提交时一次生效；取消量计数；
  300 ms 后无迟到写入；
- **空后缀 BackSpace**：吃掉并计入 `raw_ignored_edits`，不删除已提交/在途文本；
- **焦点切换**（应用一直不授予写锁）：整个待解决 run（已提交块 + 后缀）记为明确失败
  （`raw_failed_units`），不写入旧文档、不重放到新上下文；新上下文只接受自己的 `z `；
- **停用竞争**：`sink_unload_contract` 在队列中有编辑会话时停用；陈旧会话不写文档，
  TSF 释放回调后门才打开。

正常模式（引擎路径/候选菜单）的 Enter/Escape/BackSpace 未改变。

## 7. 异步启动、停用与卸载

| 场景 | 用例 | 结果 |
| --- | --- | --- |
| 启动后立即停用（启动阻塞中） | `sink_startup_lifecycle` | Activate/Deactivate 均 < 1.5 s；键直通且精确 |
| 旧启动晚于下一次激活 | 同上 | 第二个 TIP 保持字面；旧启动失败后状态 failed，不被采纳 |
| 启动失败 | `sink_engine_missing`、`sink_startup_lifecycle` | 字面精确；marker 保留；无编译产物 |
| 后台线程全部退出后可卸载 | `sink_startup_lifecycle` | 门在启动线程退出且两个 TIP 释放后变 S_OK |
| TSF 仍持有回调时拒绝卸载 | `sink_unload_contract` | 队列中会话使门为 S_FALSE；授予后 S_OK |
| 激活被拒绝不留下引擎 | `sink_strict_unregistered` | Advise 失败后不再请求启动；进程正常退出 |
| watcher/窗口/候选清理 | `sink_startup`、`product.ui.candidate_window` | Stop 后门可开；窗口销毁；元素 Stop 后回调不达控制器 |

未单独制造“部署互斥量被另一进程持有”的 60 s 等待；用**启动函数阻塞**覆盖同一
STA-free 等待形状。真实部署锁等待期间的停用没有在隔离环境执行（见 §10）。

## 8. 候选 UIElement 审查

实现未改变；新增/确认：

- 无显式选择时 `GetSelection` 返回 0（原文行），与“无选择 Enter 提交原文”一致；
- 每次 `UpdateUIElement` 都带完整 `TF_CLUIE_*` 集合（保守但正确）；`pbShow=FALSE`
  仍更新数据、不建窗口；
- 管理器在 `EndUIElement` 后仍可查询元素；只读查询继续有效；
- `Stop` 之后迟到 `SetSelection` 不达控制器；
- 候选元素/UIElement sink 计入模块存活；
- 假管理器只用于受控生命周期；**真实 `ITfUIElementMgr`、真实前台点击、真实
  `GetTextExt`/`ReadCompositionRect` 仍未验证**，不使用合成 anchor 补成通过。

## 9. 验证分层与命令

| 层 | 结果 |
| --- | --- |
| A 纯类型/参考模型 | `product.unit.key_admission`、`product.unit.settings_manifest_paths`（新增共享冲突重试单测）通过 |
| B 受控 TextStore + 直接 sink | **全部通过**：既有 143 项 + 新增 4 项（capacity_continue、unload_contract、degraded_keys、startup_lifecycle）；共 147 项，146 通过，0 失败，1 跳过（`windows.tsf_context_observer.real_focus`，无真实前台焦点） |
| C 正式注册 + StrictTsf + 真实应用 | **未执行**：没有已授权隔离环境；未注册 |

实际命令与退出码：

| 命令 | 结果 |
| --- | --- |
| `powershell -File tools/product/build_product.ps1 -SkipTests` | 0（最终身份 `0.2.3-w2br` / `08d177c`） |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error`（r9） | 0；147 项：146 通过，0 失败，1 跳过；127.18 s |
| 同上（r10，连续第二次） | 0；126.71 s |
| 同上（r11，版本提交后的最终二进制） | 0；126.45 s |
| `sink_capacity_continue`/`sink_unload_contract`/`sink_startup_lifecycle` 各重复 5 次 | 15/15 通过 |
| `sink_strict_unregistered` 重复 3 次 | 3/3 通过 |
| `sink_settings_timing` 重复 15 次（重试修复后） | 15/15 通过 |
| `stage_product.ps1 -Replace` | 0；`EpoCanva-0.2.3-w2br`，21 files，4264706 bytes，commit `08d177c` |
| `EpoCanva.exe --diagnose <report>` | 0；`package=ready`、`manifest=ok`、21/21 匹配、`tsf_registration=no` |

保留的失败/抖动日志（未删除）：`build/w2br-ctest-r1.log`（strict 在修复前崩溃）、
`r2`（unload 固定 500 ms 竞态）、`r4`（焦点阶段假设错误）、`r8`（settings error 5）。
最终通过日志：`build/w2br-ctest-r9.log`、`r10.log`、`r11.log`。

## 10. 隔离环境授权请求

没有已授权环境，未在用户主系统注册/切换输入法。最小方案与一次性请求见
`docs/windows/isolation-verification-plan.md`：

- 一台可回滚的 Windows 11 x64 VM/测试机，本地管理员，快照；
- 固定测试应用（记事本、Edge、VS Code）与固定测试数据；
- 允许临时写入 CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}` 的
  `InprocServer32` 与 `GUID_TFCAT_TIP_KEYBOARD` 分类，并通过
  `ITfInputProcessorProfiles` 注册/启用 en-US profile；
- 允许切换输入法并在测试后删除；提供回滚点、时间窗、执行人、证据目录；
- 测试矩阵：StrictTsf 前台、真实 UIElementMgr/候选点击不抢焦点、真实文本矩形、
  100/150/200% DPI、多显示器/边缘、容量同会话、停用/卸载；
- 未获授权前不执行任何注册、切换或系统级修改。

准备但**未运行**的材料：`tools/product/isolation/{capture_state,register_tip,unregister_tip,run_layer_c}.ps1`
与 `tip_profile_helper.cc`（W3 构建）。

## 11. 变更文件

新增：
- `platforms/windows/tsf_edit/module_lifetime.h`
- `tests/windows/tsf_product/crash_capture.{h,cc}`
- `docs/windows/isolation-verification-plan.md`
- `tools/product/isolation/{capture_state.ps1,register_tip.ps1,unregister_tip.ps1,run_layer_c.ps1,tip_profile_helper.cc}`

修改（源码/测试）：
- `platforms/windows/product/{CMakeLists.txt,product_io.cc}`
- `platforms/windows/product/tests/{candidate_ui_tests.cc,product_unit_tests.cc}`
- `platforms/windows/tsf/text_service.cc`
- `platforms/windows/tsf_context/{CMakeLists.txt,selection_reader.cc}`
- `platforms/windows/tsf_edit/{composition_writer.cc,edit_session.cc}`
- `platforms/windows/tsf_product/{candidate_ui.cc,product_test_seam.h,product_test_seam.cc,text_service_controller.h,text_service_controller.cc}`
- `tests/windows/tsf_edit/test_host.{h,cc}`
- `tests/windows/tsf_product/product_sink_tests.cc`

修改（文档/版本）：
- `docs/windows/{tsf-keyboard-input-contract.md,tsf-engine-lifetime-contract.md,candidate-ui-contract.md}`
- `README.md`、`platforms/windows/product/VERSION`（`0.2.3-w2br`）

## 12. 未完成、风险与门

1. **C 层未验证**：没有授权环境；正式注册的 TIP、StrictTsf 真实前台、真实 UIElementMgr、
   真实 `GetCompositionRect/GetTextExt`、DPI/多显示器仍未执行。
2. **原始 `0x270a5` 打字期 AV 未逐字复现**：已用 WER + 反汇编 + 同根因的卸载期 AV 证明
   生命周期链；精确旧测试代码不在 git。报告没有把它写成“已在输入路径复现”。
3. **部署锁等待期间的停用**：用阻塞启动覆盖同一 STA-free 形状；未制造真实的 60 s 部署
   锁等待。
4. **代理项对/astral 输入**：B 层键盘路径没有合法 astral 字符，端到端未覆盖；单位测试
   覆盖原子追加与整对回退。
5. **同文件交织的提交**：产品修复、宿主契约与回归测试放在同一代码提交（`5a5edd2`）。
   原因：新卸载门会让旧的强制 `FreeLibrary` 宿主变红，拆开会产生一个已知失败的中间
   提交；版本/文档单独提交。
6. `rime.dll` 内嵌构建路径 67/68 处、词表再分发未决、`/pathmap` 重建仍是公开分发门。
7. OneDrive I/O 抖动仍在；最终连续两次全量通过，但重复通过不等于证明所有竞态不存在。
8. 未 push/tag/amend/reset --hard/clean；本轮结束后停止，不进入安装器。

## 13. 交付状态

- 源码/测试提交：`5a5edd2bd36a029341b5dfff4da98ae5bf6a58e9`。
- 版本/包身份提交：`08d177caceaaa544b6ce00774600b63fa5fbee3b`（`0.2.3-w2br`）。
- 运行包：`build/product/stage/EpoCanva-0.2.3-w2br/`；manifest commit `08d177c`；
  `--diagnose` 实际退出码 0，21/21 文件 SHA-256 匹配。
- 诊断证据（仅开发测试目录，不入包、不上传）：
  `build/windows-product/product/run/capacity-v9/diagnostics/crash-20261029-001808-537.dmp`
  与首机会话报告；`.../capacity-v10e/diagnostics/hang-20261029-004410-880.dmp` 与报告。
- 失败与最终通过日志见 §9。
