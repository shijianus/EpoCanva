# W2-C -- 输入保真 P0：原文归宿主，建议可取消

起点 HEAD：`8474e081a28a58f99827d8a8500de00af8a396ee`（W2-B-R 文档提交）。
代码/测试提交：`519eea6c0b3a6505d7bbcba8c4ef77679497f5e1`。
版本/包身份提交：`c17e81171b01d401fd5d3bfa51ca091fd5940620`（`0.2.4-w2c`）。
本报告在随后的文档提交中；完整 SHA 见 `git log` 最新一条。
没有 push/tag/amend/reset --hard/clean；未注册系统输入法；仍为**内部运行包**。

## 1. 结论摘要

- 普通原文不再经过产品的任何队列、容量、raw 捕获或重放路径：
  文本键一律 `pfEaten=FALSE`，由宿主文档承载。
- 引擎只消费**有界、可丢弃**的建议流；建议的延迟、过期、队列满、引擎暂停
  都不会删除、回滚或改写宿主原文。
- 唯一的文档写入是用户显式选择的候选替换；写者在 SetText 前逐单元读回并比较
  caret 前的 token，不匹配就拒绝且不调用 SetText。
- W2-B-R 的 177 个字符是 `pfEaten=TRUE` 之后的内部容量丢失，不是用户取消，
  也不是宿主默认处理。旧用例的期望来自产品自身状态，所以把丢失当成了通过条件。
  本轮已更正归属（§2），并由独立参考模型替代。
- 最终全量：147 项，146 通过、0 失败、1 跳过（`real_focus`，无真实前台焦点）。
  17 项 sink 用例重复 5 次共 85 次全部通过；编辑层 30 项全部通过（含新的
  `windows.tsf_edit.replace_token` 27 个检查）。
- 包：`EpoCanva-0.2.4-w2c`，21 文件，4,265,729 字节，manifest commit `c17e811`；
  `--diagnose` 退出码 0，`package=ready`、`manifest=ok`、21/21 SHA-256 匹配、
  `tsf_registration=no`。

## 2. 1202 / 1024 / 177 的准确归属（更正 W2-B-R §5）

固定输入：`product.integration.sink_capacity_continue` 的 `ab` + 1200 × `x`。
1202 个 key-down 事件；每个键恰好 1 个 UTF-16 code unit（ASCII）。
**键事件数与文本单元数是两个单位**；本输入数值相同，但不得把它们当作同一单位相加。
真实异步文档锁被持有，首个引擎键在途。

| 归属 | 实际值 | 说明 |
| --- | --- | --- |
| 输入事件（key-down） | 1202 | 每单元一个键（另有等量 key-up，`pfEaten` 只看 down） |
| 文本 UTF-16 单元 | 1202 | 全部 ASCII |
| 已接受（`pfEaten=TRUE`） | 1202 / 1202 | 旧 `HandleRawKey` 对可捕获文本无条件 `return true` |
| 已由宿主默认处理 | **0** | 产品吃掉了所有键，宿主没有插入机会 |
| 已应用 | **1025** | 1（首个引擎键 `a`）+ 1024（raw 块）；旧报告的“1024”只算了 raw |
| 仍待应用（结算后） | 0 | 队列/在途归零，块只写一次 |
| 用户明确取消 | 0 | 没有 Escape/取消 |
| 内部丢失 | **177** | 键 1026–1202，最后 177 个 `x` |

177 的具体触发链（HEAD 源码逐行可核对）：

1. 键 1 `a` 被提交给引擎，其编辑会话因文档锁未授予而在途；
2. 键 25 触发高水位（`QueueDepth() >= kHighWater = 24`），`EnterRawMode()` 撤出
   队列尾部（键 2–24，23 单元）；
3. 后续文本键由 `CaptureRawText` 填入 `RawTextRun::kDefaultCapacityUtf16 = 1024`
   （键 2–1025，恰好 1024 单元）；
4. 键 1026 的 `Append` 返回 `kNeedsFlush`；`FlushRaw()` 因
   `HasPendingEngineInput()`（键 1 未解决）立即返回；重试仍为 `kNeedsFlush`，
   于是 `RecordRawDropped(1, 1)`、`raw_unrecoverable_++`；
5. `HandleRawKey` 忽略 `CaptureRawText` 的 false，仍然返回 true：这些键
   `pfEaten=TRUE`，但文本没有进入任何文档；共 177 个；
6. 锁释放后，键 1 的编辑与 1024 单元 raw 块依次应用，文档为 1025 单元。

丢弃原因：**容量不足 + 在途键阻塞 flush**，不是焦点变化、不是退役、不是用户取消。
旧用例的 oracle 由产品自身状态构造（`retained = burst.substr(0, retained.size())`，
再以 `retained.size() + dropped_keys == typed_units` 判通过），因此把内部丢失
合法化了；这是测试缺陷，输入保真并未通过。W2-C 删除整条 raw 路径后，同样输入为
1202/1202 宿主插入、0 内部丢失、0 待应用、文档等于完整参考（§6）。

## 3. 数据归属与状态

```
用户按键 ──OnKeyDown──> 产品只做决定
   │                         │
   │ 普通文本/Space/Enter/Backspace/Escape(无菜单)
   │                         └── pfEaten=FALSE ──> 宿主文档拥有原文（唯一权威）
   │
   └─ 显式菜单交互（↑/↓、Enter 有选择、Escape 有菜单）
          └── pfEaten=TRUE（不产生原始文本）
                 └── 选择 → 版本/身份/范围校验 → ReplaceToken（SetText 前读回比较）

建议流（可丢弃）:
  token 键 ──> SuggestionKey[64] ──> 协调器 kKey ──> 引擎 ──> CandidateSnapshot（只读）
                    │ 满/忙/过期/隐私
                    └── drop（菜单关闭；原文不受影响）
```

状态不再有“已接受但未应用”的原文：原文要么在宿主文档里，要么从未被产品吃下。
因此不存在“内部失败被算成用户取消”的账目，也不存在无限队列或写盘保存输入。

## 4. 实现变化与源码位置

| 变化 | 位置 |
| --- | --- |
| 文本键一律直通；只有菜单交互 TRUE | `platforms/windows/tsf_product/text_service_controller.cc:399`（`HandleKeyDown`）；`PredictKey` 同文件 |
| 有界 64 项建议流、单在途、重试与丢弃 | 同文件 `:572`（`SubmitNextSuggestion`）、`:627`（`ResolveSuggestionWork`） |
| 替换结果结算；不确定写入计内部丢失 | 同文件 `:765`（`ResolveReplacement`） |
| 宿主编辑只更新 caret 镜像/结束过期流 | 同文件 `:1086`（`OnHostEditCompleted`） |
| 建议模式：引擎观察永不产生文档编辑 | `platforms/windows/tsf_coordinator/coordinator.cc:1742`（`SetSuggestionMode`）、`:1908`（`ResolveEngineResult` 分支） |
| 显式替换提交（版本/identity/页索引） | `coordinator.cc:1751`（`SubmitSuggestionReplacement`）、`:1992`（`SubmitSuggestionPlan`） |
| 范围读回比较、空替换/后缀/非折叠拒绝 | `platforms/windows/tsf_edit/composition_writer.cc:647`（`ReplaceToken`） |
| 替换计划；空替换在构建期拒绝 | `platforms/windows/tsf_edit/edit_plan.cc:221`（`BuildReplaceToken`） |
| 宿主编辑不再使绑定失效（kHostOwned） | `platforms/windows/tsf_context/context_observer.cc:778` |
| 宿主编辑的 caret 镜像 | `platforms/windows/tsf_coordinator/observed_binding.cc`（`OnHostEditCompleted`） |
| 编辑会话完成标志原子化 + 同步完成回调 | `platforms/windows/tsf_edit/edit_session.cc` |
| 测试 seam 按 STA 线程区分控制器；新增建议/替换/丢失计数 | `platforms/windows/tsf_product/product_test_seam.{h,cc}` |

关键不变量：

- 只有 `ReplaceToken` 会调用 `SetText`；普通输入期间没有产品文档写。
- 建议的失败（队满、引擎忙、许可拒绝、流过期、重置失败）只影响建议。
- 替换的空文本在计划阶段拒绝；写者还有第二道空文本拒绝，永不删除原文。
- 替换只作用于“紧贴折叠 caret、且逐单元等于 expected”的范围；前面是 token 字符时拒绝。
- 建议的 version/identity/context/generation 全部校验；过期选择不写文档。

## 5. 独立参考模型与验收 A/B/C

`tests/windows/tsf_product/product_sink_tests.cc` 的 `ReferenceDoc` 只由测试自己的动作
构造（`HostInsert`、`HostBackspace`、`ReplaceToken`），不读取产品的 dropped/failed 状态，
也不从产品文档长度反推期望。每个键另有账本 `InputRoute`：

- `typed_text_units`：测试交给产品/宿主的文本单元；
- `host_inserted_units`：产品返回 FALSE 后宿主实际插入的单元；
- `product_suppressed_units`：产品返回 TRUE 吞掉的文本单元（必须为 0）。
  `CheckNoSuppressedText` 在每个 A/B/C 场景断言为 0，并打印三类计数。

| 类 | 用例 | 断言 |
| --- | --- | --- |
| A 无取消/无切换 | `sink_raw`、`sink_continuous`、`sink_capacity`、`sink_capacity_continue`、`sink_pressure`、`sink_degraded_keys` 的普通段、`sink_multi`、`sink_settings_timing`、`sink_focus_switch` | 最终文档 == 完整参考；`internal_text_losses == 0`；队列/在途 0；`product_suppressed_units == 0` |
| B 明确操作 | `sink_degraded_keys`（BackSpace/Enter/Escape/空后缀 BackSpace）、`sink_candidate`（方向键/Enter/Space/Escape）、`sink_capacity` 的候选选择 | 按公开契约计算参考；Escape 不删除原文；内部溢出不计入这些操作 |
| C 焦点/文档切换 | `sink_focus_switch`、`sink_degraded_keys` Phase E、`sink_candidate` 外部编辑、`sink_raw` 外部编辑 | 每个文档分别等于自己的参考；旧菜单版本的选择不写入子文档；旧候选不写入新文档 |

`pfEaten=TRUE` 的文字归属：所有 A/B 场景的 `product_suppressed_units` 均为 0；
唯一 TRUE 的键是菜单交互，测试对 Enter 选择显式记录 `reference.ReplaceToken`。

## 6. 压力下的完整文档与比较

最终二进制（`c17e811`）的逐检查输出（`build/w2c-final-verbose.log`）：

| 场景 | 逐键账本 | 结果 |
| --- | --- | --- |
| `sink_capacity` 1202 单元、持锁 | typed=1202 host=1202 suppressed=0 | 文档在持锁时即等于参考；释放后不变 |
| `sink_capacity` 边界 1023/1024/1025 | 累计 typed=4274 host=4274 suppressed=0 | 三段都精确等于参考；`internal_text_losses=0` |
| `sink_capacity_continue` 1202 + 2048 两轮压力 | 累计 typed=3265 host=3265 suppressed=0 | 两轮后文档精确；`internal_text_losses=0` |
| `sink_degraded_keys` backlog | typed=34 host=34 suppressed=0 | BackSpace/Enter/Space 全部是宿主效果 |
| `sink_degraded_keys` 焦点切换 | 父 45/45、子 2/2、suppressed=0 | 各文档等于自己的参考 |
| `sink_pressure` | burst 200 + sustained 100，真实建议积压 | 文档精确；无迟到重复写入 |
| `sink_continuous` | 一次就绪后 15 个词，词间不 WaitReady | 全段精确；无迟到写入 |

压力不是用“累计输入超过容量”冒充：持锁期间真实产生了建议积压
（`suggestion_queued + suggestion_dropped >= 100`），且 doc 锁/建议在途时持续输入。

## 7. 内部丢失为 0 的证据与边界

- 产品字段 `internal_text_losses` 不再是常量：它只在替换编辑以
  `kFailedPossiblyMutated`/`kFailedAfterMutation` 结束时按 expected token 长度累加；
  普通文本路径永远不会进入该计数，因为文本键不被吃下。
- 所有 A/B/C 场景断言 `internal_text_losses == 0`。
- 逐键账本 `product_suppressed_units == 0` 是另一个独立证据：它完全不读产品状态，
  只看测试自己的“键是否被吞”。
- 文档与参考逐单元比较；`retained + dropped == typed` **不再**作为通过条件。

未解决/未验证：没有制造“替换写入已发生但结果不可读”的真实故障注入，因此
`internal_text_losses` 的非零路径没有端到端用例；它是有明确增量点的运行时保护，
不是被测试证明会触发的路径。此项列为残余风险，而不是通过项。

## 8. 生命周期回归

| 用例 | 结果 |
| --- | --- |
| `sink_capacity_continue` | 通过（同会话两轮压力后模块卸载） |
| `sink_degraded_keys` | 通过（BackSpace/Enter/Escape/焦点切换） |
| `sink_startup_lifecycle` | 通过（启动阻塞中激活/停用、旧启动迟到、模块门） |
| `sink_unload_contract` | 通过；本轮改为**真实的替换编辑会话**：菜单稳定后在持锁下选择 → `session_pending != 0` 且 `guarded_objects(edit_session) >= 1`（TSF 真正持有回调）→ Deactivate 后 `DllCanUnloadNow == S_FALSE` → 释放授权后陈旧会话不写文档、门变 `S_OK`、模块释放 |
| `sink_strict_unregistered` | 通过（Advise 失败不留引擎；启动线程退出后门开） |
| `sink_startup`、`sink_engine_missing`、`sink_multi`、`sink_settings_timing`、`sink_lease_lock` | 通过 |
| `product.ui.candidate_window`、`product.ui.settings_window` | 通过 |

未重新引入：COM 引用泄漏（模块门在 TSF 持有时拒绝卸载）、错误卸载放行
（宿主只在 `S_OK` 时 FreeLibrary）、失败激活遗留引擎（strict 用例）、无界消息泵
（每次 Pump 幂等且有界）、过期候选写入（替换的版本/范围/identity 校验）。

对原 `0x270a5` 的措辞保持不变：WER 证据只证明“卸载期访问已解除映射的 vtable”
与一次本机卸载期 AV；**没有**逐字复现原始打字期 `0x270a5`，不同故障地址的复现
不能写成“原故障已直接证实”。

## 9. 命令、结果与日志

| 命令 | 结果 |
| --- | --- |
| `powershell -File tools/product/build_product.ps1 -SkipTests` | 0（最终身份 `0.2.4-w2c` / `c17e811`） |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error` | 0；147 项：146 通过，0 失败，1 跳过；113.14 s |
| 同上（前一代码状态） | 0；147 项：146 通过，0 失败，1 跳过；114.11 s |
| `ctest ... -R "sink_" --repeat until-fail:5` | 0；17 项 × 5 = 85 次全部通过；165.84 s |
| `ctest --test-dir build/p37-tsf-edit -C Release` | 0；30 项全部通过（含 `windows.tsf_edit.replace_token` 27 检查） |
| `ctest ... -R "sink_capacity$|sink_capacity_continue|sink_unload_contract|sink_degraded_keys" -V` | 0；证据见 §6/§8 |
| `powershell -File tools/product/stage_product.ps1 -Replace` | 0；`EpoCanva-0.2.4-w2c`，21 文件，4,265,729 字节，commit `c17e811` |
| `EpoCanva.exe --diagnose <report>` | 0；`package=ready`、`manifest=ok`、21/21、`tsf_registration=no` |

保留的失败/中间日志（未删除）：`build/w2c-ctest-r1.log`（迁移前 4 项失败）、
`build/w2c-edit-build-r5.log`（临时插桩编译失败，已移除插桩）、以及各轮构建日志。
最终通过日志：`build/w2c-final-ctest.log`、`build/w2c-final-verbose.log`、
`build/w2c-repeat-r1.log`、`build/w2c-edit-ctest-r12-full.log`。

## 10. 提交、包与源码身份

- 代码/测试：`519eea6c0b3a6505d7bbcba8c4ef77679497f5e1`
  （20 文件，+2515/-1588；产品修复与对应回归在同一提交，因为新测试依赖新路径）。
- 版本/包：`c17e81171b01d401fd5d3bfa51ca091fd5940620`（`0.2.4-w2c`）。
- 文档（本报告与契约更正）：文档提交，见 `git log` 最新一条。
- 运行包：`build/product/stage/EpoCanva-0.2.4-w2c/`；`package.manifest` 的
  commit 为 `c17e811`；`--diagnose` 21/21 SHA-256 匹配。
- 包内不含输入日志、dump、PDB 或测试 DLL；只有 `EpoCanva.exe`、
  `epocanva_tsf.dll`、`rime.dll`、数据/词表/许可证/声明。
- 构建路径告警仍在：`rime.dll` 内嵌 67/68 处机器路径（公开分发门，未在本轮处理）。

## 11. 未完成、风险与阻塞

1. **C 层未执行**：没有已授权的隔离环境；正式注册 TIP、StrictTsf 真实前台、
   真实 `ITfUIElementMgr`、真实 `GetTextExt` 仍未验证。本报告不以 B 层通过冒充 C 层。
   具体授权请求沿用 `docs/windows/isolation-verification-plan.md`：一台可回滚的
   Windows 11 x64 VM/测试机（本地管理员、快照）、固定测试应用与数据、允许临时写入
   CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}` 的 `InprocServer32` 与
   `GUID_TFCAT_TIP_KEYBOARD` 分类并注册/启用 en-US profile、测试后删除；
   未获授权前不执行任何注册、切换或系统级修改。本轮没有重复让用户运行 Lab。
2. **候选窗口锚点**：建议模式下文档没有产品组合，`UpdateCandidateUi` 不读
   `GetTextExt`；真实自绘窗口的位置/显示属于未验证项。受控测试走宿主绘制路径。
3. **替换后当前 token 的建议**：重置引擎流后，当前 token 的剩余字符可能不再产生菜单
   （产品不保存 token 文本用于重放）；下一个 token 正常。只影响建议，不影响原文。
4. **host-edit 信用模型**：应用把多次插入合并为一次编辑、或 BackSpace 在文档开头无效时，
   直通键的信用可能剩余，使下一次外部编辑被当作预期编辑。只影响建议流。
5. **内部丢失非零路径**没有真实故障注入用例（§7）；它是保护计数器，不是已验证触发。
6. **代理项对**：B 层键盘路径没有合法 astral 输入；原子追加/回退由编辑层与
   `product.unit.key_admission` 覆盖，未用合成事件冒充端到端。
7. `rime.dll` 内嵌构建路径与词表再分发仍是公开分发门。
8. OneDrive I/O 抖动仍在；连续通过不证明所有竞态不存在。
9. `platforms/windows/tsf_product/key_admission.{h,cc}`（`KeyAdmissionQueue` /
   `RawTextRun`）仍编译进 direct-sink 变体并由 `product.unit.key_admission` 测试，
   但**已不在产品控制器路径上**；测试 seam 的 `raw_*` 字段恒为 0。删除这个遗留库
   及其测试属于后续清理，不在本轮 P0 范围。
10. 未 push/tag/amend/reset --hard/clean；本轮结束后停止，不进入安装器。

## 12. 变更文件

源码：
- `platforms/windows/tsf_context/context_observer.{cc,h}`
- `platforms/windows/tsf_coordinator/{coordinator.cc,coordinator.h,observed_binding.cc,observed_binding.h}`
- `platforms/windows/tsf_edit/{composition_writer.cc,composition_writer.h,edit_plan.cc,edit_plan.h,edit_session.cc}`
- `platforms/windows/tsf_product/{product_test_seam.cc,product_test_seam.h,text_service_controller.cc,text_service_controller.h}`

测试：
- `tests/windows/tsf_context_observer/observer_test_support.{cc,h}`
- `tests/windows/tsf_edit/{CMakeLists.txt,composition_tests.cc}`
- `tests/windows/tsf_product/product_sink_tests.cc`

文档/版本：
- `docs/windows/tsf-keyboard-input-contract.md`（W2-C 契约；§6 更正归属）
- `docs/windows/candidate-ui-contract.md`（选择改为显式替换）
- `docs/reports/W2-B-R-crash-and-lifecycle.md`（追加 §5.1 更正）
- `docs/reports/W2-C-input-fidelity.md`（本报告）
- `platforms/windows/product/VERSION`（`0.2.4-w2c`）
