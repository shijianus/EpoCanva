# W3-A -- 隔离注册、正式 TSF 与真实应用验证（停在授权门）

起点 HEAD：`6fc499d2dc51c704bc63e2719d391a5c0304b432`（W2-C 文档提交）。
代码/测试提交（候选位置锚定、故障注入、建议重同步）：
`0d00e351880530d552b1eef3156c1b5cd7df2379`。
隔离身份与注册工具提交：`b96d28e62a7bacfeb75ec254620e7f879535ff4d`。
版本/包提交：`28d52d7293593cd2edab02418366d48a0a58dffa`（`0.2.5-w3a`）。
本报告所在文档提交：由最终文档提交记录（见 §10）。
没有 push/tag/amend/reset --hard/clean；没有在主系统注册/切换输入法；仍为**内部运行包**。

## 1. 结论摘要

- **C 层未执行**：本机仍没有已授权的专用 Windows 环境（§2）。没有注册、激活、切换或
  真实应用验证；本报告不以任何 B 层通过冒充 C 层。
- B 层补齐了候选替换的故障注入：写前拒绝、读回不一致、排队后宿主继续编辑、同文本
  他处、焦点切换、`SetText` 已改文本但返回失败；全部以测试自己的文档参考为 oracle。
- 发现并修正了一个真实的位置问题：引擎观察可能早于宿主最后一次编辑通知，快照里的
  caret 会落后一个通知；写入前的 live caret 校验正确地拒绝了它，但菜单也不应使用落后
  的位置。现在菜单展示时读取真实 caret 作为该版本的锚点，选择请求携带该锚点，写入器
  再次要求 live caret、范围和文本全部一致。
- 建议流丢项后不再把剩余按键当作完整前缀：`token_abandoned` 保持到 token 边界；
  边界后新 token 正常。显式替换/Escape 之后的继续输入按“当前 caret 的新建议 token”处理
  （不重放旧前缀）。
- 注册工具使用正式 CLSID + 正式 en-US profile `{7D3E2A94-...}` + `GUID_TFCAT_TIP_KEYBOARD`；
  写入带所有权标记、逐项读回、失败回滚；移除只清理本产品内容并验证残留。工具已构建；
  只在本机运行了只读 `status`（`com_registration=absent`、`profile_present=no`）。
- 最终包：`EpoCanva-0.2.5-w3a`，21 文件，4,268,289 字节；`--diagnose` 退出 0，
  `package=ready`、`manifest=ok`、21/21 SHA-256 匹配、`tsf_registration=no`。

## 2. 环境只读核查与具体授权请求

### 2.1 本机事实（2026-10-29）

| 项 | 观测 |
| --- | --- |
| OS | Windows 11 Pro 10.0.26200 build 26200，x64，Lenovo 83DG 物理机 |
| 虚拟化 | `HypervisorPresent=False`；无 VBoxManage/vmrun/qemu |
| Sandbox/Hyper-V | **无法确认**：`Get-WindowsOptionalFeature` 需要提权，当前会话非管理员；不把提权失败写成“功能不存在” |
| 权限 | 当前用户不是本地管理员 |
| 授权 | 无专用 VM/测试机、无快照；未注册 CLSID、未切换输入法 |
| 注册状态 | `status`：HKLM/HKCU COM 均 absent；profile 不存在；无活动 TIP |
| DLL 架构 | 产品构建为 x64；注册脚本以 PE 头 `0x8664` 再次校验 |
| 自动执行/截图 | B 层测试可创建真实窗口并截图；没有已授权的 C 层应用自动化 |

### 2.2 一次性授权请求（需要用户明确同意）

1. **目标环境**：一台可回滚的 Windows 11 x64 VM/测试机（本地管理员、已有快照），
   不是生产主机。若使用 Windows Sandbox/Hyper-V，启用功能本身需提权并可能重启，
   必须单独授权；本轮没有启用任何虚拟化。
2. **系统变更**：
   - `HKLM\Software\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}\InprocServer32`
     （默认值 = 暂存 DLL，`ThreadingModel=Apartment`）；
   - 同 CLSID 的 `Implemented Categories\{34745C63-B2F0-4784-8B67-5E12C8701A31}`；
   - `ITfInputProcessorProfiles::Register` + `AddLanguageProfile`（en-US，
     profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`）；
   - `ActivateProfile` / `DeactivateProfile`（当前会话）；测试期间切换键盘。
3. **注册/移除范围**：仅上述 CLSID、profile、类别；不修改其他输入法或默认布局；
   测试后用工具移除并对比注册表快照。
4. **提权/重启**：HKLM 与 profile 注册需要管理员；预计不需要重启。不要求关闭安全软件、
   安装证书或下载系统镜像。
5. **目录映射/网络**：复制仓库快照或 stage + 工具到隔离机；产品运行期无网络 I/O，
   可离线；共享目录中的产品数据只读，证据手工取回。
6. **回滚**：先做快照；`reg export` 记录 CLSID/类别；工具失败自动回滚自己的写入；
   异常时恢复快照。
7. **证据**：`capture_state.ps1` 前后快照、`status` 输出、注册表导出、日志、截图、
   `--diagnose`；固定测试输入；不读取用户既有文档/凭据；不上传、不自动回传。

**未获授权前不执行任何注册、切换或系统级修改。** 本文件、已构建的工具和暂存包都不是授权。

## 3. 候选替换故障注入（B 层，已执行）

新用例：`product.integration.sink_replacement_faults`。它加载真实 `epocanva_tsf.dll`
（direct-sink 变体）、真实 msctf 线程/文档/上下文与真实 `CompositionEditSession`；
测试自己维护 `ReferenceDoc`，断言最终文档逐单元相等，产品计数器只作为证据。

| 场景 | 注入 | 结果 |
| --- | --- | --- |
| 菜单后 caret 移到同文本另一处 | 真实 TSF 选区变更 | 旧版本选择被拒；无产品 SetText；文档不变 |
| 替换排队时宿主继续输入 | `SetAsyncLocks(true)` + 两次宿主插入 | 过期计划不覆盖新输入；SetText 增量只有宿主的 2 次 |
| 菜单后 token 被改成同长文本 | 外部 client 替换 `hel`→`hex` | 读回不一致拒绝；无产品 SetText |
| 替换排队时焦点切到子上下文 | `CreateChildContext` | 父/子文档都不接收写入；父上下文重新绑定后继续 |
| 写入已改文本但返回 `E_FAIL` | `MutationFault::kEFailAfterMutation` | 不回滚、不重复；`internal_text_losses += 3`；上下文门关闭；宿主普通输入继续；新上下文恢复候选 |

证据（`build/w3a-faults-verbose.log`）逐项通过，包括：

- 写前失败/拒绝不改原文；
- 过期操作不覆盖新输入；
- 每个场景的文档与独立参考完全一致；
- 不确定写入只发生一次 SetText，且没有被补偿性回滚；
- 不确定状态下的后续选择不写文档，宿主文本不被吞（`product_suppressed_units=0`）；
- 新上下文可重新绑定、显示菜单并安全替换自己的 token。

编辑层新增（`windows.tsf_edit.replace_token`）：

- `hel x hel`，计划锚定第二个 token（caret 9），宿主把 caret 移到 3 →
  `kSelectionMoved`，`SetText` 调用数不变；同一字符串不被当成同一 token；
- 选择不可读 → 拒绝（`kNoSelection`），文档不变；
- 原有的同长不同文本、后缀、空替换、非折叠选择继续覆盖。

## 4. 宿主原文模式的定位与重同步（B 层，已执行）

### 4.1 定位来源

- 旧实现只在 `coordinator.composition_active` 时读组合矩形；原文归宿主后没有组合，
  因此真实自绘窗口没有锚点。该假设已移除。
- 现在菜单版本首次展示时用只读 `GetSelection` 读取真实折叠 caret（UTF-16 ACP），
  只读位置，不读文本；矩形来自 `ITfRangeACP::SetExtent(caret - input, input)` +
  `ITfContextView::GetTextExt`。没有布局/矩形/不可见时隐藏窗口，不使用鼠标坐标。
- 引擎观察可能早于宿主最后一次编辑通知。实测中快照 caret 为 19、真实 caret 为 20；
  新的写入前 live caret 校验拒绝了替换（这是正确的），但菜单也不应锚在 19。现在展示时
  的真实读取会把锚点重同步到 20，并回填协调器镜像。

### 4.2 选择校验

- 快照版本、快照 identity、当前绑定 identity、页内索引；
- 菜单锚点 caret（真实读取）；
- 写入会话内：live caret == 锚点、caret 前 `expected` 长度的范围逐单元等于引擎输入、
  前一个字符不是 token 字符、选择折叠、替换文本非空；
- 任一失败都不调用 SetText。相同字符串在别处、光标移动、外部编辑、旧版本点击都不会
  改错 token。

### 4.3 丢项后的重同步

`product.integration.sink_suggestion_resync`：单 token 120 键突发使有界建议队列溢出；
`token_abandoned` 保持，剩余输入被丢弃且不开菜单；Space 边界清除标记；新 token 正常
打开菜单并完成替换。`internal_text_losses=0`，原文逐单元等于参考。

## 5. 注册身份与最小工具

- `platforms/windows/tsf/identifiers.{h,cc}` 新增正式 en-US profile
  `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`，与 CLSID 一样冻结。
- `epocanva_tip_profile_helper`：`register/unregister/activate/deactivate/status`；
  每个命令打印 HRESULT 并回读验证；`status` 分开报告 COM 注册、profile 存在和活动
  键盘 TIP。默认构建不包含它（`EPOCANVA_BUILD_ISOLATION_TOOLS=OFF`）。
- `register_tip.ps1`：授权开关、管理员检查、PE x64、导出检查（`dumpbin` 可用时）、
  拒绝非本产品 CLSID、写入所有权标记、逐项读回、失败回滚、JSON 证据。
- `unregister_tip.ps1`：只删除带标记或指向 `epocanva_tsf.dll` 的内容；移除后验证无残留；
  保留其他值/子键。
- 本轮在本机只运行只读 `status`：COM absent、profile absent、无活动 TIP。没有运行
  `register`、`activate`、`register_tip.ps1` 或 `unregister_tip.ps1`。

## 6. C 层与 DPI（未执行，不得写成通过）

| 项 | 状态 |
| --- | --- |
| 正式注册/激活/移除 | 未执行（无授权环境） |
| StrictTsf 真实前台 + 记事本/第二应用 | 未执行 |
| 真实 `ITfUIElementMgr` Begin/Update/End | 未执行 |
| 真实 `GetTextExt`/`ReadTokenRect` 锚点 | 未执行 |
| 点击候选不抢焦点 | 未执行 |
| 100/150/200% DPI、多显示器/边缘 | 未执行（B 层只有受控布局/截图） |
| 冷启动/失败/停用/应用关闭 | 部分 B 层覆盖；C 层未执行 |
| 注册移除后再启用 | 未执行 |

“全部按键直通、原文没丢”只证明输入保真，**不**单独算输入法功能通过。C 层必须在
授权环境里证明真实候选能安全改变目标 token。

## 7. 命令与结果（实际执行）

| 命令 | 退出码 / 结果 |
| --- | --- |
| `git rev-parse HEAD` | 起点 `6fc499d2...` |
| `powershell -File tools/product/build_product.ps1 -SkipTests` | 0（多轮；编译错误已修复，日志保留） |
| `ctest --test-dir build/windows-product -C Release --output-on-failure --no-tests=error` | 0；149 项：148 通过、0 失败、1 跳过（`windows.tsf_context_observer.real_focus`） |
| 同上，`-R "sink_candidate$|sink_replacement_faults|sink_suggestion_resync" --repeat until-fail:3` | 0；3 项 × 3 = 9 次全部通过 |
| `ctest --test-dir build/p37-tsf-edit -C Release` | 0；30/30 |
| 隔离工具 configure（`-DEPOCANVA_BUILD_ISOLATION_TOOLS=ON`） | 0 |
| `cmake --build ... --target epocanva_tip_profile_helper` | 0 |
| `epocanva_tip_profile_helper.exe status` | 0；COM/profile 均 absent |
| PowerShell 解析检查（4 个隔离脚本） | 0 |
| `powershell -File tools/product/stage_product.ps1 -Replace` | 0；`EpoCanva-0.2.5-w3a`，21 文件，4,268,289 字节 |
| `EpoCanva.exe --diagnose` | 0；`package=ready`、`manifest=ok`、21/21、`tsf_registration=no` |
| `git diff --check` | 0 |

保留的关键证据日志：`build/w3a-faults-verbose.log`、`build/w3a-resync-r1.log`、
`build/w3a-candidate-r16.log`、`build/w3a-final-build-ctest.log`、
`build/w3a-final-stage.log`、`build/w3a-final-diagnose.txt`、`build/w3a-edit-ctest-r3.log`。
失败的中间日志（构建/测试）未删除；`build/w3a-build-r1..r21.log`、`build/w3a-candidate-r1..r15.log`
记录了定位过程中被拒绝的中间状态。

## 8. 未完成、风险与阻塞

1. **C 层整体未执行**：没有授权环境。阻塞项是 §2.2 的授权请求；未授权前不注册、不切换、
   不做真实应用自动化。
2. **真实 DPI/多显示器**：B 层布局测试不能替代；未验证。
3. **宿主通知合并/延迟**：菜单展示时的真实 caret 读取已覆盖“快照落后一个通知”的实测
   情况；如果真实宿主把选择变化合并到更晚的 OnEndEdit，展示时读取到的 caret 就是锚点，
   之后的移动由写入器拒绝。没有在真实应用中验证这一点。
4. **替换后继续输入**：显式替换/Escape 会重置引擎流；不输入边界而继续输入时，后续字符
   是“从当前 caret 开始的新建议 token”，只建议/替换后缀。这是已知语义，不是丢字。
5. **不确定状态恢复**：上下文按真实 TSF context 锁存，恢复路径是新上下文；同一 context
   不会自动解除。这与 P3.9 的不确定写入策略一致，但真实应用中的“新上下文”形态需要在
   C 层验证。
6. **注册工具未在隔离机执行**：脚本已通过语法检查和构建，但没有真实的注册/移除证据。
7. `rime.dll` 内嵌构建路径（67/68 处）仍是公开分发门；OneDrive I/O 抖动仍存在。
8. 本轮没有制作安装器；包内没有 dump/PDB/测试 DLL/注册工具。

## 9. 交付清单

- 起点：`6fc499d2dc51c704bc63e2719d391a5c0304b432`
- 代码/测试：`0d00e351880530d552b1eef3156c1b5cd7df2379`
- 隔离身份/工具：`b96d28e62a7bacfeb75ec254620e7f879535ff4d`
- 版本/包：`28d52d7293593cd2edab02418366d48a0a58dffa`（`0.2.5-w3a`）
- 文档：本报告所在提交 + 最终记录提交（§10）
- 包：`build/product/stage/EpoCanva-0.2.5-w3a/`，21 文件，4,268,289 字节
- 授权环境：无；具体请求见 §2.2
- 注册/激活/移除证据：只有只读 `status`（未注册）
- 真实应用输入/候选交互：未执行
- 截图/最终文档比较：B 层故障用例逐单元比较通过；C 层无
- DPI/多屏实际覆盖：无（C 层未执行）
- 回滚/清理：主系统未改；隔离工具只读；包暂存可删除
- git status：提交后应为 clean；见最终文档提交

## 10. 变更文件

产品/测试（`0d00e35`）：
- `platforms/windows/tsf_coordinator/{candidate_snapshot.h,coordinator.cc,coordinator.h}`
- `platforms/windows/tsf_edit/{composition_writer.cc,composition_writer.h,edit_plan.cc,edit_plan.h,edit_result.h,edit_session.cc}`
- `platforms/windows/tsf_product/{candidate_ui.cc,candidate_ui.h,product_test_seam.cc,product_test_seam.h,text_service_controller.cc,text_service_controller.h}`
- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/tests/candidate_ui_tests.cc`
- `tests/windows/tsf_edit/{composition_tests.cc,test_host.cc,test_host.h}`
- `tests/windows/tsf_product/product_sink_tests.cc`

隔离身份/工具（`b96d28e`）：
- `platforms/windows/tsf/{identifiers.h,identifiers.cc}`
- `tools/product/isolation/{register_tip.ps1,unregister_tip.ps1,tip_profile_helper.cc}`

版本：
- `platforms/windows/product/VERSION`（`0.2.5-w3a`，`28d52d7`）

文档：
- `docs/windows/candidate-ui-contract.md`
- `docs/windows/tsf-keyboard-input-contract.md`
- `docs/windows/isolation-verification-plan.md`
- `docs/reports/W3-A-isolation-registration.md`（本文件）
