# W3-A -- 正式 TSF 注册与本机真实应用测试（trace 构建最小闭环通过；正式包复验待 UAC）

起点 HEAD：`6fc499d2dc51c704bc63e2719d391a5c0304b432`（W2-C 文档提交）。
代码/测试提交（候选位置锚定、故障注入、建议重同步）：
`0d00e351880530d552b1eef3156c1b5cd7df2379`。
隔离身份与注册工具提交：`b96d28e62a7bacfeb75ec254620e7f879535ff4d`。
版本/包提交：`28d52d7293593cd2edab02418366d48a0a58dffa`（`0.2.5-w3a`）。
本报告初始文档提交：`187c924dd3b7b76615ff1a2f5bb0b1b8a445b7d0`；本轮契约/包身份
文档提交：`e9e7038`。本行及随后的纯文档修正只记录 SHA 与最终 git 状态，不改变二进制。
本轮起点 HEAD 为 `3de9adfe21fd7e8817c39cd7c363aeac7a19687c`；随后提交
`b9386f4`（UIElement 契约、token 锚点、有界重试、B 层测试）和 `488b6f5`
（UIElement 能力类别注册工具）。冻结正式包从 `488b6f5` 构建；本报告及两份契约文档
提交为 `e9e7038`（随后的纯文档修正不改变二进制），代码工作区在构建后无改动。
没有 push/tag/amend/reset --hard/clean；没有留下主系统注册（测试后已移除并验证）；
仍为**内部运行包**。VM 已关闭，未再启动。

## 1. 结论摘要

- **C 层 trace 构建曾在本机跑通一次候选闭环**（2026-10-30 15:18；用户明确拒绝
  VM/第三方平台）。正式 TIP 注册/激活/移除成功且无残留；TIP DLL 与 `rime.dll` 均加载进
  全新 Notepad；输入 `alpha hel` 后出现产品自绘候选窗口；Down 两次选择第 1 页
  （`held`），Enter 显式提交，文档变为 `alpha held`，焦点仍在原文档，候选窗口关闭；
  清理后 `cleanup_verified=True`。证据 `build/w3a-c-local/evidence6/`。详见 §11。
  **这份 trace 成功不能作为正式链路证据**：stage8 在 `BeginUIElement` 返回
  `pbShow=FALSE` 时仍然自绘，违反 TSF UILess 的宿主可见性决定（见 §11.4/§11.5）。
- **正式（非 trace）链路已按契约修正并冻结**。当前工作区修正：
  `pbShow=FALSE` 对所有宿主一律不显示自绘窗口，删除 Notepad 进程名白名单；TIP 注册
  `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`；`FALSE` 后立即发送首次全量 `UpdateUIElement`；
  无选择 `GetSelection` 返回 `S_FALSE`；`Show(FALSE)` 隐藏真实窗口并使 `IsShown=FALSE`。
  冻结包 `build/product/stage/EpoCanva-0.2.5-w3a-r3/`，源码 `488b6f5`，DLL SHA-256
  `911B63A3FB79BFC26A873E879A4A9B5661A4DE938121D1961AB78C62CC5E8294`，manifest 21/21，
  trace 关闭、导出仅 `DllGetClassObject`/`DllCanUnloadNow`（§11.6）。用正式包的闭环复验
  尚未执行，必须等一次明确的 UAC 授权（§11.10）。
- 本轮审查并修正的契约/安全缺陷：
  1. **UIElement 显示契约**：不再按进程名覆盖宿主 `show=FALSE`；宿主决定优先，Notepad
     “返回 FALSE 但不绘制”作为宿主兼容限制记录，不用白名单掩盖（§11.4）。
  2. **live caret 边界**：菜单锚点只接受 `token 起点 + 宿主 token UTF-16 单元数`；快照
     caret 可落后一个通知（8→9），选择时重读 live caret 但只接受该 token 结束位置。旧
     菜单绝不重定位到任意当前光标，写入器校验不放松（同文本他处、移动、非折叠选区、
     外部编辑均拒绝；§11.4.2、§11.9）。
  3. **锚点重试生命周期**：布局/选区瞬时不可读时按 pacing 重试，预算 40 次/宿主 token，
     不因候选版本变化重置；token 边界、目标绑定/丢失、`Show(TRUE)` 才重置；耗尽后关闭
     菜单，不留下不可见的可键盘选择菜单，不忙等、不在 UI 线程等待布局（§11.9）。
  4. **observer 选择读取**：WinUI 瞬时 `kSessionUnavailable`/`kNoSelection` 的有界重试
     （20 次/绑定），避免无休止 10 ms 唤醒。
- 测试选择协议修正：引擎候选第 0 页是 RAW（原文 `hel`），一次 Down 会“成功替换成
  原文”而看不出变化；测试改为 Down 两次选择第 1 页 `held`，以文档变化作为通过判据。
- B 层补齐了候选替换的故障注入：写前拒绝、读回不一致、排队后宿主继续编辑、同文本
  他处、焦点切换、`SetText` 已改文本但返回失败；全部以测试自己的文档参考为 oracle。
- 建议流丢项后不再把剩余按键当作完整前缀：`token_abandoned` 保持到 token 边界；
  边界后新 token 正常。显式替换/Escape 之后的继续输入按“当前 caret 的新建议 token”处理
  （不重放旧前缀）。
- 注册工具使用正式 CLSID + 正式 en-US profile `{7D3E2A94-...}` + `GUID_TFCAT_TIP_KEYBOARD`
  + `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`（`{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}`）；
  写入带所有权标记、逐项读回、失败回滚；移除只清理本产品内容并验证残留。工具已构建；
  2026-10-30 在本机执行注册/激活/移除，最终 `status` 为 COM/profile absent、active no。
  **旧的 trace/正式 -r2 注册轮次没有写 UIElement 能力类别**；修正后的包必须重新注册并
  在 `capture_state.ps1` 中确认两个类别都存在。
- 旧正式包 `EpoCanva-0.2.5-w3a-r2`（DLL `1EB48D19...92AA`）是**待验证构建**，且其
  UIElement 实现仍含进程名白名单，不能称为正式链路通过；本轮已提交
  `b9386f4`/`488b6f5`，并从 `488b6f5` 重建不可变正式包 `EpoCanva-0.2.5-w3a-r3`
  （DLL `911B63A3...E8294`，身份见 §11.6）。旧 `-r2` 包保留为对照，不覆盖仍被用户
  应用映射的目录。

## 2. 环境核查与精确授权申请（2026-10-29，只读）

本节全部为实际执行的只读命令与观测。没有注册、没有切换输入法、没有启动任何 VM 或
Sandbox、没有修改 BCD 或系统功能。当前会话是过滤令牌（未提升）；一次 UAC 提权请求被
取消，因此改用不需要提权的 `Win32_OptionalFeature` 完成了功能状态查询。核查日志：
`build/w3a-envcheck/readonly*.txt`。

### 2.1 本机事实

| 项 | 观测 | 依据 |
| --- | --- | --- |
| 主机 | Windows 11 Pro 10.0.26200 build 26200，x64，Lenovo 83DG，31.7 GiB RAM，i7-14650HX（16C/24T） | `Win32_OperatingSystem` / `Win32_ComputerSystem` / `Win32_Processor` |
| 本地管理员 | 用户 `Smart Kevin` 在 `Administrators` 组；当前进程令牌只含 `BUILTIN\Users`（未提升） | `net localgroup Administrators` + `WindowsIdentity` |
| 硬件虚拟化 | 固件已启用 VT-x：`VirtualizationFirmwareEnabled=True`、`VMMonitorModeExtensions=True`、`SLAT=True`、`DEP=True` | `Win32_Processor` / `Get-ComputerInfo` |
| Hypervisor | **未运行**：`CsHypervisorPresent=False`、`HyperVisorPresent=False`；`systeminfo` 输出四项硬件需求而不是“已检测到虚拟机监控程序”；VBS=0 | `Get-ComputerInfo` / `systeminfo` / `Win32_DeviceGuard` |
| Windows Sandbox | 功能 **Enabled**（`Containers-DisposableClientVM` InstallState=1）；`WindowsSandbox.exe`/`WindowsSandboxClient.exe` 存在（10.0.26100.8875）。Sandbox 依赖 Hyper-V；当前 hypervisor 未运行，**预期无法启动**，需先启用 hypervisor 启动项并重启后实测（未执行） | `Win32_OptionalFeature` + 文件版本 + `systeminfo` |
| Hyper-V | `Microsoft-Hyper-V-All`/`-Hypervisor`/`-Services`/`-Management-PowerShell` 均 Enabled；`vmms` 服务 Running；但 hypervisor 未运行 | `Win32_OptionalFeature` + 服务 |
| 其他功能 | `VirtualMachinePlatform` Enabled；`HypervisorPlatform` **Disabled**；WSL Enabled（无发行版） | `Win32_OptionalFeature` + `wsl --status` |
| 待重启 | CBS `RebootPending`/`PackagesPending`/`RebootInProgress`=False；WU `RebootRequired`=False；PFRO 有 86 条非空 delete-on-reboot（Chrome Temp、ClickToRun 更新、13 个 Windows 字体文件等），没有正在等待的 CBS/WU 服务操作 | 注册表只读 |
| 现有 VM | VMware Workstation 17.6.4；inventory 6 条，3 个本地 vmx：`Documents\...\MyWindows11 US-Program`（今日 21:23 修改、无快照文件、10-24 的锁）、`D:\Program Files (x86)\...\MyWindows11 US-Program`（2 个 2026-08-04 的 `.vmsn`/`.vmem`、`.vmsd` 加密、`vmrun listSnapshots` 无输出）、`UbuntuServerMyTest`；E:/F: 条目缺失。一个 `vmware-vmx` 进程自 2026-10-24 13:07 起运行，而 `vmrun list` 为空，状态不一致；未触碰任何 VM | 文件/注册表只读 |
| 本地安装介质 | `Win11_24H2_Chinese_Simplified_x64.iso` 5,559 MB、`Windows10.iso`、`windows.iso`（108.3 MB，VMware Tools）位于 VMware Workstation 目录；**无需下载系统镜像** | 目录只读 |
| VMware CLI | `vmcli` 17.6.4 支持 `VM Create`、`Power`、`MKS captureScreenshot/sendKeyEvent/sendKeySequence`、`Snapshot Take/Revert/Clone`、`Guest copy/run`（Guest 需 Tools + 登录）；**没有 mouse 事件命令** | `vmcli <module> --help` |
| 宿主自动化 | 会话 1、交互；UIA 可读 14 个顶层窗口；截图成功（探针 PNG 7,817 B）；键鼠注入 API 可加载，但无法把私有窗口置前，**未发送任何按键**。宿主 UIA 看不到 Sandbox/VM 内部 | 实际探针 |
| 产品包（2026-10-29） | stage 21 个载荷 + `package.manifest` = 22 文件 4,270,468 B；`epocanva_tsf.dll` SHA-256 `4011A585...C92FC4F`；helper `BAFAB758...C97A14`。**当前正式包已更新为 `-r2`（`1EB48D19...92AA`），见 §11.6** | `Get-FileHash` |
| 注册状态 | `status`：HKLM/HKCU COM 均 absent；profile 不存在；无活动 TIP | 只读 `status` |

结论：本机没有已确认“专用且可回滚”的 Windows VM；Windows Sandbox 功能已启用，但
hypervisor 未运行；本机同时有 Win11 24H2 ISO 和 VMware `vmcli`，因此可以在**不改主机
功能、不重启**的前提下新建一台专用 VM。

### 2.2 精确执行申请（三选一；已被 2026-10-30 本机测试取代，VM/Sandbox 未获授权且被拒绝）

**方案 A（推荐，需授权）——新建专用 VMware VM**

1. **目标环境**：`D:\EpoCanvaIsolation\W3A-Win11`，Windows 11 24H2 x64，4 vCPU / 8 GiB /
   40 GiB 薄置备磁盘，UEFI；vTPM 使用测试专用口令（只写入本机 gitignored 证据文件，
   不进仓库）；网卡创建后保持断开。宿主不改功能、不提权、不重启，只新增该目录（≤40 GiB）。
2. **安装**：从本机 `Win11_24H2_Chinese_Simplified_x64.iso` 安装，`autounattend.xml`
   建本地管理员 `epocanva`、跳过在线账户与网络 OOBE；安装 `windows.iso` 中的 VMware Tools；
   关机后取快照 `w3a-clean`。若 24H2 的 TPM 检查无法用 vTPM 满足，则在无人值守文件里
   使用明确的 TPM 绕过并在报告中记录，不隐式改宿主。
3. **注册位置**：只在 guest 内写
   `HKLM\Software\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}\InprocServer32`
   （默认值 = guest 内暂存 DLL，`ThreadingModel=Apartment`）、同 CLSID 的
   `Implemented Categories\{34745C63-B2F0-4784-8B67-5E12C8701A31}`，并调用
   `ITfInputProcessorProfiles::Register` + `AddLanguageProfile`（en-US，
   `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`）与 `ActivateProfile`/`DeactivateProfile`。
   **宿主注册表不写。**
4. **网络与目录**：VM 网卡断开；guest 内 `C:\w3a\input` 是只读 HGFS 共享
   （host `D:\EpoCanvaIsolation\input`，`SetWriteAccess=false`；只放暂存产品包、隔离工具、
   脚本和期望哈希），`C:\w3a\output` 是唯一可写 HGFS 共享（`SetWriteAccess=true`）；
   不映射仓库、OneDrive、桌面或用户目录。
5. **自动化**：`vmcli MKS` 发送键盘序列/截图；登录后由 `vmcli Guest run`（Tools + 本地账户）
   运行 guest 内 PowerShell，使用 UIA、`SendInput`、`GetGUIThreadInfo` 和窗口截图完成
   真实应用（记事本 + 第二应用）的输入、候选选择、焦点/文本/矩形证据。鼠标点击候选由
   guest 内脚本执行（vmcli 无 mouse 命令）。
6. **证据取回**：`vmcli MKS captureScreenshot` 的 guest 截图、`vmcli Guest copyFrom` 或
   可写 HGFS 输出目录取回注册表导出、`status` JSON、文本逐单元比较、焦点/矩形日志和包哈希
   （sha256sum 在 guest 内计算并回读比对）；统一保存到 `build/w3a-c-layer-evidence/`
   （gitignored）。不上传、不自动回传。
7. **清理/回滚**：测试后 `Snapshot Revert w3a-clean`，关机；必要时删除整个 VM 目录；
   宿主无注册表/输入法改动可回滚。

**方案 B（仅在你会指定并确认时）——使用现有 Win11 VM**

需要你给出：VM 名称/路径、确认它可作为一次性测试环境、VM 加密口令与该 guest 的管理员
口令；若它正在运行，先由你关闭。开始前在 VMware 中确认快照或新建 `w3a-clean`；没有
可用快照就不开始。注册/网络/证据/回滚同方案 A。风险：该 VM 是你的个人环境，可能含个人
数据；未确认前不使用，也不读取其中的用户文档。

**方案 C（需要主机提权 + 重启）——Windows Sandbox**

1. **系统变更**：`bcdedit /set {current} hypervisorlaunchtype auto`（管理员）+ 重启；
   若要让 VMware 与 Hyper-V 共存，还需启用 `HypervisorPlatform`（再次重启）。
   这两步都不在本轮现有授权内，必须单独批准。
2. **Sandbox 配置**：`Networking=Disable`、`ClipboardRedirection=Disable`、
   vGPU/Audio/Video/Printer 关闭；映射 `D:\EpoCanvaIsolation\input` 只读、
   `D:\EpoCanvaIsolation\output` 可写；不映射仓库、OneDrive、桌面或用户目录；
   `LogonCommand` 运行输入目录中的测试脚本。
3. **注册位置**：只在 Sandbox 内；关闭 Sandbox 即销毁。宿主除 hypervisor 启动项外无改动；
   回滚 = 关闭 Sandbox，必要时恢复 `hypervisorlaunchtype`。
4. **限制**：Sandbox 只有一个虚拟显示，DPI/多屏覆盖有限；宿主 UIA 无法驱动内部，
   必须由 `LogonCommand` 自驱，证据经可写映射目录取回。

**未获授权前不执行任何 VM/Sandbox 启动、注册、切换或系统级修改。** 本文件、已构建的
工具和暂存包都不是授权。

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

### 3.1 `internal_text_losses += 3` 的归因

`internal_text_losses` **不是“丢字个数”**，而是“产品无法证明结果、按整段 token 保守计入的
UTF-16 单元数”。代码路径：

- `ProductTextController::ResolveReplacement` 只在
  `kFailedPossiblyMutated` 或 `kFailedAfterMutation` 时执行
  `internal_text_losses_ += snapshot.last_suggestion_expected_units`
  （`platforms/windows/tsf_product/text_service_controller.cc:794-799`）；
- `last_suggestion_expected_units_ = expected_text.size()`
  （`platforms/windows/tsf_coordinator/coordinator.cc:2008`），单位是 **UTF-16 code unit**；
- 在 `product.integration.sink_replacement_faults` 场景 E 中，被替换的 token 是 `hel`
  （3 个 UTF-16 单元），注入 `MutationFault::kEFailAfterMutation`：测试 store 先改文本、
  再返回 `E_FAIL`（`tests/windows/tsf_edit/test_text_store.cc:506-511`）。因此一次不确定写入
  记 `+3`，即 **1 次事件 × 3 单元**，不是 3 次丢字，也不是 3 个字符被吞。

该场景的实际文档结果是：测试自己的 `ReferenceDoc` 把 `hel` 替换为候选文本，断言文档逐单元
等于参考（`tests/windows/tsf_product/product_sink_tests.cc:2793-2795`），并且只发生一次
`SetText`（同文件 2796-2797）。也就是说，**文档实际上包含替换结果**；产品只是因为写调用
失败而无法证明这一点，才把整段 token 报为“可能丢失”。这四类必须分开：

| 类别 | 判定 | 计数器/证据 |
| --- | --- | --- |
| 确认文本丢失 | 宿主文本没有进入文档；独立参考与文档不一致；或产品吃掉了文本键却没有交付 | 测试的 `CheckDocExact` 失败、`product_suppressed_units != 0`；A/B 层现有场景均为 0 |
| 写入结果不确定 | 写调用已发出或已成功、随后失败，无法读回证明最终文本 | `kFailedPossiblyMutated`/`kFailedAfterMutation`；`internal_text_losses += 整段 expected units`（场景 E 的 `hel` = 3） |
| 过期操作拒绝 | caret 移动、文本不一致、焦点切换、版本过旧；写前校验拒绝，未调用 `SetText` | 场景 A-D；`SetText` 调用数不变，文档等于参考 |
| 用户明确取消 | Escape 关闭可取消建议；不删除用户已输入文本，不写文档 | `text_service_controller.cc:457-469`；不增加 `internal_text_losses` |

C 层目前没有“真实应用里强制写入失败”的手段；这一计数在 C 层只可能由真实 TSF 写失败触发，
不能靠注入。报告不得把 `internal_text_losses` 的非零值直接称为“确认丢字”。

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
- 2026-10-30 在本机实际运行了 `register_tip.ps1`、helper `register/activate/deactivate/
  unregister` 和 `unregister_tip.ps1`；每轮结束后 `status` 验证 COM/profile absent、
  active no。注册/输入只在授权的本机测试轮次内发生，详见 §11.2。

## 6. C 层与 DPI（未执行，不得写成通过）

| 项 | 状态 |
| --- | --- |
| 正式注册/激活/移除 | **已执行**（本机临时 HKLM；trace 轮次与正式工具均成功；最终 absent，`cleanup_verified=True`） |
| StrictTsf 真实前台 + 新 Notepad | **trace 构建曾通过**（stage8 自绘，违反 `pbShow` 契约）；修正后的正式 -r3 包复验待一次 UAC，不得写成通过 |
| 真实 `ITfUIElementMgr` Begin/Update/End | **trace 观测到** `BeginUIElement` 返回 `show=FALSE`；stage8 自绘是诊断捷径，正式链路严格遵从 FALSE（保留元素、首次 Update、不自绘）；修正后的真实宿主行为待复验 |
| 真实 `GetTextExt`/`ReadTokenRect` 锚点 | **trace 执行过**：token 矩形 `[caret-input, caret)`；现在锚点只接受 token 起点+宿主单元数，布局重试 40 次/token，仍无布局则关闭菜单，不伪造矩形 |
| 候选选择/仅替换目标 token | **trace 构建曾通过**：`alpha hel` → Down×2 选第 1 页 `held` → Enter → `alpha held`；`alpha ` 前缀保留，焦点仍在文档。修正后的正式包若宿主不自绘则不会出现候选窗口，必须在选择前停止 |
| 点击候选不抢焦点 | 未执行（本轮用键盘选择）；候选窗口为 `WS_EX_NOACTIVATE`，选择期间前台始终是 Notepad |
| 100/150/200% DPI、多显示器/边缘 | 未执行（B 层只有受控布局/截图） |
| 冷启动/失败/停用/应用关闭 | 部分 B 层；C 层执行了激活/停用/移除，未测菜单打开时应用关闭 |
| 注册移除后再启用 | profile 多轮注册/移除成功，但未在同一轮验证“移除后立刻重新启用并再次显示候选” |

“全部按键直通、原文没丢”只证明输入保真，**不**单独算输入法功能通过。trace 构建的
C 层闭环已经证明真实候选能安全改变目标 token；正式包必须复验后才可写成发布态通过。
真实 DPI/多屏、1202 输入、移动/粘贴/外部编辑、过期候选、设置生效时机等仍按 §11.6
的顺序执行，不能由本轮单点通过代替。

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
| `powershell -File tools/product/stage_product.ps1 -Replace`（2026-10-29） | 0；`EpoCanva-0.2.5-w3a`，21 文件，4,268,289 字节（已被 2026-10-30 的 `-r2` 取代） |
| `EpoCanva.exe --diagnose`（2026-10-29 包） | 0；`package=ready`、`manifest=ok`、21/21、`tsf_registration=no` |
| `git diff --check` | 0 |
| `build/w3a-envcheck/envcheck-readonly{1..9}.ps1` | 0（每轮）；事实见 §2.1；日志 `build/w3a-envcheck/readonly*.txt` |
| `Start-Process -Verb RunAs ... envcheck-elevated.ps1` | UAC 被取消；未获得提升令牌，未执行提权查询 |
| `vmcli <module> --help` | 0；VM Create/Power/MKS/Snapshot/Guest 能力见 §2.1 |
| `cmake --build build/w3a-isolation-tools --config Release --target epocanva_tsf_product` | 输出 `epocanva_tsf.dll`（多次；构建命令经管道到 `tail`，未单独记录 cmake 退出码） |
| `ctest --test-dir build/windows-product -C Release -R "product.integration.engine_settings\|product.integration.deployment_snapshot"` | 0；2/2 通过（0.74 s / 9.79 s） |
| `cmake --build build/windows-product --config Release --target epocanva_tsf_product` | 输出正式 `epocanva_tsf.dll`；最终 SHA-256 `1EB48D19...92AA`（host-specific 兼容修复后重建） |
| `tools/product/stage_product.ps1 -StageRoot ...EpoCanva-0.2.5-w3a-r2 -Replace` | 0；21 载荷文件、4,269,313 字节，required/forbidden 检查通过 |
| `ctest -R "product.ui.candidate_window\|sink_candidate\|sink_replacement_faults\|sink_suggestion_resync"`（历史 trace/正式 -r2 轮次） | 0；4/4 通过（当时 host-specific 自绘策略下的受控契约） |
| `ctest --no-tests=error`（修正后 Release，149 项） | 0；148 passed、0 failed、1 skipped（`windows.tsf_context_observer.real_focus`）；日志 `build/w3a-full-ctest-final-*.log` |
| 全新 configure/build（`build/w3a-formal-r3-product`，`EPOCANVA_PRODUCT_TRACE=OFF`） | 0；`epocanva_tsf.dll` SHA-256 `911B63A3...E8294`；dumpbin 导出仅 `DllGetClassObject`/`DllCanUnloadNow` |
| `stage_product.ps1 ... EpoCanva-0.2.5-w3a-r3` | 0；21 载荷文件、4,273,921 字节，required/forbidden 检查通过 |
| `EpoCanva.exe --diagnose`（r3 stage） | 0；`package=ready`、`manifest=ok`、21/21、`commit=488b6f5` |
| `c_layer_agent.ps1` / `trigger.ps1 -Elevate`（trace stage8） | 0；`agent_passed=True`、`cleanup_verified=True`、`overall=True` |
| `trigger.ps1 -Elevate -Formal`（正式 r2，两次） | 7；两次 UAC 均被取消，注册未发生，未输入 |
| PowerShell 解析检查（4 个测试脚本，改动后） | 0（每轮） |

保留的关键证据日志：`build/w3a-faults-verbose.log`、`build/w3a-resync-r1.log`、
`build/w3a-candidate-r16.log`、`build/w3a-final-build-ctest.log`、
`build/w3a-final-stage.log`、`build/w3a-final-diagnose.txt`、`build/w3a-edit-ctest-r3.log`。
失败的中间日志（构建/测试）未删除；`build/w3a-build-r1..r21.log`、`build/w3a-candidate-r1..r15.log`
记录了定位过程中被拒绝的中间状态。

## 8. 未完成、风险与阻塞

1. **正式（非 trace）包复验未执行**：trace stage8 的闭环是违约诊断捷径（§11.4），不能
   算正式通过。修正后的冻结包 `EpoCanva-0.2.5-w3a-r3`（源码 `488b6f5`，DLL
   `911B63A3...E8294`）已构建、manifest 21/21、trace OFF，但尚未注册/输入；
   `trigger.ps1 -Formal` 的历史两次 UAC 均被取消（退出码 7）。正式包复验前，C 层最小
   闭环只能写“trace 诊断通过”，不能写“正式包通过”。
2. **真实 DPI/多显示器**：B 层布局测试不能替代；未验证。
3. **宿主通知合并/延迟**：锚点已改为 token 起点 + 宿主 token UTF-16 单元数；快照 caret
   可落后一个通知（8→9），但任意其他 caret（包括另一处相同文本）都拒绝/关闭菜单，
   不重定位、不扩大写入授权。布局/选区重试按 token 有界（40 次），不因候选版本变化
   重置。尚未在第二真实宿主上验证，也未验证鼠标点击候选（本轮只用键盘 Down/Enter）。
   **宿主绘制限制**：Notepad 在 trace 轮次返回 `show=FALSE` 且未绘制；正式链路不再按
   进程名自绘。若第二个宿主也有同样缺陷，候选会隐藏；这是记录在案的宿主限制，受控测试
   仍保证“宿主绘制时无第二窗口”。
4. **替换后继续输入**：显式替换/Escape 会重置引擎流；不输入边界而继续输入时，后续字符
   是“从当前 caret 开始的新建议 token”，只建议/替换后缀。这是已知语义，不是丢字。
5. **不确定状态恢复**：上下文按真实 TSF context 锁存，恢复路径是新上下文；同一 context
   不会自动解除。这与 P3.9 的不确定写入策略一致，但真实应用中的“新上下文”形态需要在
   C 层验证。
6. **注册工具已在本机执行**：多轮 `register_tip.ps1` + helper 注册/激活，测试后 deactivate/
   unregister；最终 `status` absent，`cleanup_verified=True`。VM/Sandbox 未使用（用户拒绝）。
7. **旧正式 stage 目录无法完全清理**：`build/product/stage/EpoCanva-0.2.5-w3a` 的
   `epocanva_tsf.dll`/`rime.dll` 仍被 Explorer/Edge/Chrome/Qoder 进程映射（这些进程在
   早前轮次加载过该 TIP），Windows 拒绝删除/覆盖。未结束用户进程；当前正式包改为同级
   `EpoCanva-0.2.5-w3a-r2`。旧目录是构建产物残留，**不是注册残留**；待相关应用重启后可
   删除。此限制必须在正式验收时说明。
8. **引擎启动失败会在本会话保持 literal**：一次失败的异步启动（本轮曾因 trace stage4/5
   只复制 DLL、缺少同目录 `data/` 而失败）会让控制器在本 TIP 会话内保持无引擎；修正包布局
   后同一测试 attach 成功。产品是否需要“启动失败后自动重试”尚未决定；当前不以自动重启
   掩盖配置/部署错误。
9. **Notepad 会话恢复/多标签**：新 Notepad 会恢复旧测试标签并在启动后切换上下文；测试
   使用唯一空文件、空闲门控前台切换、逐键前台/修饰键检查，并在文本不等于注入串时停止。
   这是测试环境的限制，不是产品文本丢失。
10. `rime.dll` 内嵌构建路径（67/68 处）仍是公开分发门；OneDrive I/O 抖动仍存在。
11. 本轮没有制作安装器；包内没有 dump/PDB/测试 DLL/注册工具。正式包不含 trace 代码路径
    （`EPOCANVA_PRODUCT_TRACE=OFF`）。

## 9. 交付清单

- 起点：`6fc499d2dc51c704bc63e2719d391a5c0304b432`
- 代码/测试：`0d00e351880530d552b1eef3156c1b5cd7df2379`
- 隔离身份/工具：`b96d28e62a7bacfeb75ec254620e7f879535ff4d`
- 版本/包：`28d52d7293593cd2edab02418366d48a0a58dffa`（`0.2.5-w3a`）
- 文档：`187c924dd3b7b76615ff1a2f5bb0b1b8a445b7d0`；最终记录提交为随后的纯文档提交
- 正式包：`build/product/stage/EpoCanva-0.2.5-w3a-r2/`，21 载荷文件；
  `epocanva_tsf.dll` SHA-256 `1EB48D19F637B1964EDA11A765EC8EF39E576B810D7C65B0982E2E6EE24A92AA`；
  manifest 21/21 匹配；不含 trace。
- 诊断 trace 包：成功轮次使用 `build/w3a-trace-stage8/`（DLL
  `BBDE6FA95696F3E95794C046CEEE8530044A6D48353B12B1867AD27C21570FFE`）；策略收紧后
  重建 `build/w3a-trace-stage9/`（DLL
  `E41980DB191B3BEFA57936E95A6BC90321CD47745755B71EBAC25AFC56506093`），尚未重跑。
  **trace 包只用于定位，不是交付物。**
- 授权环境：未使用 VM/Sandbox（用户明确拒绝）；2026-10-30 在本机做临时 HKLM 注册测试
- 注册/激活/移除证据：`build/w3a-c-local/evidence6/`（`registration.json`、`removal.json`、
  `status-pre.txt`、`status-post.txt`、`helper-*.txt`、`elevated-summary.txt`）
- 真实应用输入/候选交互：**trace 构建最小闭环通过**；`text_before_typing=alpha hel`、
  `text_after_selection=alpha held`、`selection_method=keyboard_down_enter`、
  `prefix_alpha_kept=true`、`focus_kept_after_selection=true`、`passed=true`
- 截图/最终文档比较：`evidence6/after-typing-1s.png`、`before-selection.png`、
  `candidate-window.png`（8 项：RAW `hel`，COMP `held/helicopter/helium/hell/hello/helm/helmet`）、
  `after-selection.png`（`alpha held`）
- trace 证据：`evidence6/trace.txt` 中成功 PID 的关键阶段：`menu_arrow_selection=1`、
  `finalize_live_caret=9`、`replacement_page=1`、`replacement_outcome=1`、
  `replacement_edit_status=1`、`replacement_edit_reason=0`。
- 正式包复验：**未执行**（UAC 取消，退出码 7；无注册、无输入）
- DPI/多屏实际覆盖：无
- 回滚/清理：已完成并验证无 HKLM/HKCU 产品键、无 profile、无 active TIP、无本轮测试
  Notepad/文件；VM 已关闭。旧正式 stage 的 DLL/rime 仍被用户应用映射，无法删除（§8.7）。
- git status：HEAD 不变；本报告与隔离方案文档为未提交的工作区修改；无 staged 改动

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

2026-10-30 本机定位与契约修正的**源码提交**：
- `b9386f4`：`EPOCANVA_PRODUCT_TRACE` 选项（默认 OFF；正式包不定义）；trace-only 的
  key sink/阶段记录；`pbShow=FALSE` 对所有宿主一律不自绘并补首次全量 Update、
  `Show(FALSE)/IsShown`、无选择 `S_FALSE`；token 起点+宿主单元数的锚点、异位/非折叠/
  外部编辑/新 token 拒绝、40 次/token 布局重试；observer 选择读取 20 次/绑定重试；
  B 层测试与 seam 字段。
- `488b6f5`：注册工具补 `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`，注册/移除/状态捕获
  都读回两个类别。
- 文档（本报告、候选 UI 契约、隔离验证方案）提交为 `e9e7038`（后续纯文档修正不改变
  二进制）；冻结包从 `488b6f5` 构建，构建后源码工作区无改动。
- 测试代理/证据在 `build/w3a-c-local/`、trace 包在 `build/w3a-trace-stage3..8/`，
  均在 gitignored `build/` 下，不是产品交付物。

## 11. 本机隔离测试（2026-10-30，VM/Sandbox 被明确拒绝）

用户明确拒绝继续使用 VM/第三方平台，要求在本机隔离测试。VM 已关闭，未再启动。本机
测试使用**临时 HKLM 注册**，测试后立即移除并验证；未改默认输入法、未改宿主其他设置。
宿主输入语言测试前为 zh-CN（`0x08040804`），测试后已恢复为 zh-CN。

### 11.1 身份与环境

| 项 | 观测 |
| --- | --- |
| 主机 | Windows 11 Pro 10.0.26200，session 1；新 Windows 11 Notepad（WinUI，`RichEditD2DPT`），Edge 122 |
| 测试应用 | 本轮新建的唯一空文件 + 全新 Notepad 进程；不操作用户已有窗口或文档 |
| trace 包 | 成功轮次：`build/w3a-trace-stage8/`，DLL SHA-256 `BBDE6FA9...570FFE`；策略收紧后：`build/w3a-trace-stage9/`，DLL SHA-256 `E41980DB...06093`（完整包布局） |
| 正式包（旧） | `build/product/stage/EpoCanva-0.2.5-w3a-r2/`；DLL SHA-256 `1EB48D19...92AA`（待验证构建，含白名单，未复验） |
| 冻结正式包（本轮） | `build/product/stage/EpoCanva-0.2.5-w3a-r3/`；源码 `488b6f5`；DLL SHA-256 `911B63A3...E8294`；manifest 21/21；复验未执行 |
| 权限 | 注册/激活/移除通过一次 UAC 提权；测试 agent 与 Notepad 始终普通权限（`is_elevated=False`） |
| 输入语言 | Notepad 线程切到 en-US `0x04090409`；TIP DLL 与 `rime.dll` 随后加载 |
| 前台门 | 空闲 ≥0.9 s 才由提权 relay 尝试置前；每个按键前检查前台根窗口仍是本轮 Notepad；修饰键变化或文本不等于注入串立即停止 |

### 11.2 注册、激活与清理（实际执行）

- `register_tip.ps1 -Hive HKLM -AuthorizedIsolationEnvironment`：写入 CLSID/InprocServer32/
  category/marker，逐项读回成功；evidence `registration.json`。
- `epocanva_tip_profile_helper.exe register`：`Register CLSID 0x00000000`、
  `AddLanguageProfile en-US 0x00000000`；profile 在数秒后变为 `profile_present=yes`、
  `profile_enabled=yes`。**profile 可见性是异步的**：立即查询为 no，轮询后才为 yes。
- `helper activate`：`ActivateProfile 0x00000000`。helper 自身线程的语言是 zh-CN，
  `GetActiveProfile` 不报告本 profile；把 Notepad 线程切到 en-US 后 TIP DLL 才加载。
  因此“helper 线程 active=no”不能单独判失败，真实判据是目标应用是否加载 TIP。
- 引擎：`rime.dll` 加载进 Notepad；`attach_ok=1`；候选快照 8 项；stage4/5 曾因缺少同目录
  `data/` 导致引擎启动失败，改成完整包布局后 attach 正常（§8.8）。
- 清理：`helper deactivate` → `helper unregister` → `unregister_tip.ps1`；随后删除空的
  `Implemented Categories` 父键和空 CLSID 基键。最终 `status`：HKLM/HKCU COM absent、
  profile absent、active no；`cleanup_verified=True`。
- 每用户路径已实测并排除：`AddLanguageProfile`（跳过机器级 `Register`）和
  `InstallLayoutOrTipUserReg` 均返回 S_OK，但 `GetProfile` 仍 absent；TSF profile 存储按
  机器级读取。helper 的 `register-user` 试验命令和 `register_tip_user.ps1` 已删除，源码恢复。

### 11.3 第一轮失败（已定位并修复）

第一轮真实 Notepad：TIP/引擎加载、en-US、焦点、`alpha hel` 文本均正确，但 10 秒内没有
候选窗口；当时的结论是“候选 UI 未出现”。trace 定位后确认失败阶段依次是：候选被产品
自己隐藏（`show=FALSE` 误判）、菜单锚点 caret 停在 8 而宿主已到 9（`kSelectionMoved`）、
token 矩形瞬时不可读后没有重试，以及测试一次 Down 选中 RAW 页。修复与成功证据见 §11.4、
§11.5；第一轮的 `evidence5` 保留为对照，不再作为当前结论。后续轮次在
`build/w3a-c-local/evidence6/`；这些路径在 `build/` 下，被 git 忽略。

### 11.4 定位到的第一个失败阶段与修复

每一步都用 trace stage（`EPOCANVA_TRACE|stage|value`，不记录文本）定位，不靠猜测扩大测试。

1. **候选窗口在 `show=FALSE` 时被产品隐藏：这是契约要求，不是误判**。trace 显示真实
   引擎已产生 8 项候选、`candidate_token_rect_ok=1`、`candidate_anchor_present=1`，而
   `ITfUIElementMgr::BeginUIElement` 返回 `show=FALSE`。按 TSF UILess 文档，TIP 此时
   **不得**显示自己的 UI；旧代码隐藏窗口是正确行为。stage8 为了让诊断轮次看到候选，改成
   对所有 `FALSE` 自绘（`ui_host_drawn_draw_own=1`），这是**违反契约的诊断捷径**，不能
   进入正式链路。随后收紧成 Notepad 进程名白名单，仍然违反“不得按进程名忽略宿主显示
   决定”的审查要求，故本轮**彻底删除**。正式链路现在：保留真实 `ITfUIElement`、回答
   所有查询、在 `FALSE` 后立即发送首次全量 `UpdateUIElement`、不显示自绘窗口。
   Notepad 返回 `FALSE` 且未绘制候选列表是**宿主兼容限制**，证据见
   `build/w3a-c-local/evidence6/trace-hostdrawn-before.txt`（`ui_host_drawn|1`、
   `ui_host_drawn_hide|1`）；不得用白名单恢复自绘。
2. **菜单锚点 caret 落后于宿主最后一次编辑**。第一次替换 trace：
   `finalize_anchor_caret=8`、`replacement_caret=8`，写入器返回
   `replacement_outcome=3`（`kRejected`）、`edit_status=2`（`kRejected`）、
   `edit_reason=18`（`kSelectionMoved`）。文档实际有 9 个 UTF-16 单元
   （`alpha hel`），宿主在菜单展示后把 caret 从 8 推进到 9。本轮修复不是“每次更新都
   重定位到当前 caret”，而是：记录 token 起点（第一个字符通过宿主前的投影 caret），
   只接受 `起点 + 宿主 token UTF-16 单元数` 作为锚点；展示/选择时读取 live caret，但
   只有等于该 token 结束位置才接受。快照 caret 可落后（8），真实结束 9 被接受；移动到
   另一处相同 `hel`（位置不是该 token 结束）被拒绝，写入器仍按该 caret 校验 token 文本。
   受控回归：`product.ui.candidate_window` 的 anchor 策略（8→9 接受、异位拒绝、未知
   token 失败关闭）；真实 TSF sink 的候选用例中观测到 `snapshot_caret=19`、
   `doc_caret=20`，选择仍只替换一次；`sink_replacement_faults` A（同文本他处）拒绝。
3. **token 矩形瞬时不可读导致窗口永久隐藏**。宿主布局尚未完成时 `ReadTokenRect`
   失败，旧代码只隐藏、不重试；现在在菜单仍有效且矩形缺失时按 pacing 重试，仍无布局则
   继续隐藏，绝不伪造矩形。**预算为 40 次/宿主 token**，不因候选快照版本变化而重置；
   token 边界、目标绑定/丢失、宿主 `Show(TRUE)` 才重置。预算耗尽仍无矩形 → 关闭菜单，
   不留下不可见的可键盘选择菜单。受控测试断言 40 次后第 41 次被拒绝。
4. **测试选择了 RAW 页**。候选第 0 页是 RAW `hel`；一次 Down 选择它并“成功替换为
   原文”，文档看不出变化。测试改为 Down 两次选择第 1 页 `held`；替换后才要求文档
   变化。

### 11.5 成功证据（trace 构建）

成功轮次 Notepad PID 191152，trace stage8，结果字段：

| 字段 | 值 |
| --- | --- |
| `text_before_typing` / `text_after_typing` | `alpha hel` / `alpha hel` |
| `candidate_visible` / `candidate_rect` | true / `129,435,306,744` |
| 候选内容（截图） | RAW `hel`；COMP `held, helicopter, helium, hell, hello, helm, helmet`；page 1 |
| `selection_method` | `keyboard_down_enter` |
| `text_after_selection` | `alpha held` |
| `focus_after_selection_is_child` / `foreground_after_is_notepad` | true / true |
| `candidate_visible_after` | false |
| `prefix_alpha_kept` / `focus_kept_after_selection` | true / true |
| `passed` / `agent_exit` / `overall` | true / 0 / true |

trace 关键阶段：`menu_arrow_selection=1`、`finalize_live_caret=9`、
`replacement_page=1`、`replacement_submit_status=0`（accepted）、
`replacement_outcome=1`（applied）、`replacement_edit_status=1`（applied）、
`replacement_edit_reason=0`、`replacement_expected_units=3`。说明只替换了目标 token
`hel`（3 个 UTF-16 单元），`alpha ` 前缀未动；选择后焦点仍在 `RichEditD2DPT` 文档内。
本轮 `internal_text_losses` 未增加（`outcome=applied`，不是不确定写入）。

成功轮次使用 stage8（当时对所有 `show=FALSE` 自绘）；这是**违反 UILess 契约的诊断
构建**，不是正式链路。随后把策略收紧为仅 `Notepad.exe` 自绘（stage9/正式 `-r2`），仍然
违反“宿主决定优先”的契约，故本轮删除。`alpha held` 的结果证明引擎、token 替换、焦点
保持和清理链路可用，但**不能证明** `pbShow` 协商正确，也不能证明正式包通过；正式包必须
用修正后的 `show=FALSE` 严格遵从、UIElement 类别注册和首次 Update 重新验证。

### 11.6 冻结正式包身份与复验状态

本轮从提交后的源码重新 configure/build，不复用旧 build 目录，也不覆盖被用户进程映射的
旧 stage：

| 项 | 值 |
| --- | --- |
| 源码提交 | `488b6f5b7823c3265965809cdb7a312b50a8e268`（`488b6f5`） |
| stage（绝对路径） | `D:\Users\Smart Kevin\OneDrive\Desktop\EpoCanva\build\product\stage\EpoCanva-0.2.5-w3a-r3` |
| 版本 | `0.2.5-w3a` |
| `epocanva_tsf.dll` SHA-256 | `911B63A3FB79BFC26A873E879A4A9B5661A4DE938121D1961AB78C62CC5E8294` |
| `EpoCanva.exe` SHA-256 | `EB3124511EA6B3E69463B6012B62D1855C4E277F950EDB73DE3F215608F98BD9` |
| `rime.dll` SHA-256 | `BF44CDB6D3680F02B509A0859B02AB383CC507339FDE64B8C41B7DA753934041` |
| manifest | 21/21 匹配，`manifest_failures=0`，`commit=488b6f5` |
| 诊断开关 | `EPOCANVA_PRODUCT_TRACE=OFF`；DLL 中 `EPOCANVA_TRACE` 字符串计数 0 |
| 导出 | 仅 `DllGetClassObject`、`DllCanUnloadNow`（dumpbin，2 个名称） |
| 构建目录 | `build/w3a-formal-r3-product/`（全新 configure，`--diagnose` 退出 0） |
| 证据 | `build/w3a-formal-r3-configure.log`、`build/w3a-formal-r3-product-build.log`、`build/w3a-formal-r3-stage.log`、`build/w3a-formal-r3-diagnose.txt` |

旧 `EpoCanva-0.2.5-w3a-r2/` 及 `build/windows-product/` 保留为对照/测试构建，不覆盖。

- 复验命令（待授权）：`trigger.ps1 -Elevate -Formal -StageDir <r3 绝对路径>`。
- 两次历史 UAC 均被取消（触发器退出 7），未注册、未启动 Notepad、未输入；因此
  **正式包闭环仍未验证**。取消后 `status` 仍为 COM/profile absent。
- 正式复验必须在用户批准一次 UAC 后执行；不得用 trace 或 B 层结果代替正式包结论。

### 11.7 清理与本轮残留

- 注册：每轮测试后 `helper deactivate` → `helper unregister` → `unregister_tip.ps1`；
  最终 `status`：HKLM/HKCU COM absent、profile absent、active no。
- 进程/文件：每轮结束关闭本轮启动的 Notepad 及其 launcher，删除唯一测试文件；本轮结束时
  无遗留测试 Notepad/文件。早前轮次遗留的两个无窗口 Notepad（PID 141144/147448，其中
  147448 仍映射 `rime.dll`/`epocanva_tsf.dll`）已关闭并记录在
  `evidence6/leftover-cleanup.txt`。
- 输入语言：测试线程使用 en-US，结束后恢复；宿主预加载语言列表未被修改。
- 旧正式 stage `EpoCanva-0.2.5-w3a` 的 DLL/rime 仍被用户应用映射，无法删除/覆盖；当前
  正式包为 `-r2`（§8.7）。这是构建产物残留，不是注册残留；未结束用户进程。
- 诊断 trace 包/目录（`build/w3a-trace-stage3`…`stage8`）与 `build/w3a-c-local/` 均在
  gitignored `build/` 下，保留为证据，不是交付物。

### 11.8 本轮 UIElement 契约审查与 live caret 安全边界（2026-10-30）

**契约审查记录**（源码 `platforms/windows/tsf_product/candidate_ui.cc`、SDK
`msctf.h` 10.0.26100.0、官方 [UILess Mode Overview](https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview)）：

| 检查点 | 实现/证据 | 结论 |
| --- | --- | --- |
| `pbShow` 调用前初值 | `BOOL show = TRUE;` 后调用 `BeginUIElement` | 正确；**不是**自己传 FALSE 再误判宿主拒绝 |
| `BeginUIElement` HRESULT / 元素 id | 失败或 id==0 → `begin_failures`、不显示、释放元素 | 正确 |
| 调用后值 | `host_drawn_ = show == FALSE`；所有宿主一律遵从 | 删除进程名白名单 |
| `ActivateEx` 标志 | `flags` 存入 `activation_flags_`；trace `activate_flags`；行为不分支 | 仅记录，不声称 UILess 模式 |
| 注册能力类别 | 键盘类别 + `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` `{49D2F9CF-...}` | 旧 `-r2`/trace 缺失，本轮补上 |
| 回调链 | 宿主实现自己的 `ITfUIElementSink`；TIP 的 sink 不改 `pbShow`，只用于 End/Update 通知 | 宿主决定源是 `BeginUIElement` 返回值 |
| Begin/Update/End | Begin 一次；后续 Update；End 释放；宿主 End 幂等 | 正确 |
| 首次全量更新 | `BeginUIElement` 返回 FALSE 后立即 `UpdateUIElement`，标志含 DOCUMENTMGR/COUNT/SELECTION/STRING/PAGEINDEX/CURRENTPAGE | 之前缺失，本轮修正 |
| `Show(FALSE)`/`IsShown` | Show(FALSE) 隐藏真实窗口；IsShown 反映真实可见性；Show(TRUE) 经 pacing 唤醒重新读 anchor | 之前只记录布尔值，本轮修正 |
| 无选择 `GetSelection` | 返回 `S_FALSE`，`*puIndex` 无效；Finalize 不隐式接受原文 | 之前返回 S_OK/0，本轮修正 |

**live caret 边界**：锚点身份 = 快照版本 + context/generation + token 起点 + 宿主 token
UTF-16 单元数；只有 `token 起点 + units` 的 live caret 被接受。任何其他 caret（包括另一处
相同 `hel`）都是另一个 token，菜单立即失效，不重定位。选择时再次读取 live caret；写入器
`CompositionWriter::ReplaceToken` 的 live-caret 相等、token 逐单元比较、前导 token 字符
检查和标量边界检查全部保留，没有放松。

回归（本轮实际运行，B 层）：
- `product.ui.candidate_window`：`pbShow` 初值、宿主 FALSE、FALSE 后首次全量 Update、
  Show(FALSE)/IsShown、Show(TRUE) 交回、无选择 S_FALSE、无选择 Finalize、宿主 End、
  迟到回调、anchor 策略（8→9 接受、异位 `hel` 拒绝、未知 token 失败关闭）、40 次/41 次
  重试预算。
- `product.integration.sink_candidate`：真实控制器/协调器/Tsf 编辑层；观测到
  `snapshot_caret=19`、真实 caret 20 时选择仍成功且只替换一次；旧版本选择不改变文档；
  输入新 token 后旧菜单不能提交。
- `product.integration.sink_replacement_faults`：A 同文本他处拒绝；B 排队后宿主继续编辑
  不覆盖；C token 文本变化读回拒绝；D 焦点切换不写入任一文档；E SetText 成功但报错的
  不确定状态；F 非折叠选区拒绝且无 SetText。
- `product.integration.sink_suggestion_resync`：丢弃后剩余按键不当作完整前缀。
- 完整产品回归（Release，`ctest --no-tests=error`，149 项）：**148 passed、0 failed、
  1 skipped**（`windows.tsf_context_observer.real_focus`，环境限制跳过）。日志：
  `build/w3a-full-ctest-final-*.log`；失败日志未覆盖（本轮没有失败）。

**锚点重试生命周期**：`CandidateAnchorRetryBudget` 40 次/宿主 token；`candidate_anchor_retry_`
只在当前 token 的菜单仍需展示时置位；快照版本变化不重置预算；token 边界、目标绑定/丢失、
宿主 Show(TRUE) 重置；无布局/无法读 caret 时窗口隐藏并按 pacing 重试；预算耗尽关闭菜单；
失焦/停用/`Stop` 清空 anchor 与 pacing；迟到唤醒不能重开旧窗口（版本、token、dismissed
version 与 anchor 检查都在展示前）。observer 的 WinUI 瞬时选择读取重试另有 20 次/绑定预算，
避免无休止唤醒。

### 11.9 下一步（严格顺序）

1. 用冻结正式包 `-r3` 重跑一次最小闭环（一次 UAC，§11.10）；通过前不扩大矩阵。
2. 最小闭环正式通过后，按任务书依次执行：1202 输入与技术 token；移动光标/粘贴/外部
   编辑；过期候选拒绝；真实 `ITfUIElementMgr` 全生命周期；真实 token 矩形/无布局隐藏；
   启动/停用/关闭；设置生效时机；环境实际支持的 DPI/多屏。每项都保持焦点门、仅操作
   本轮窗口、结束后清理并验证。
3. 每个失败先修实际缺陷；不以 B 层通过冒充 C 层；不制作安装器。
4. 报告继续区分四类：确认文本丢失、写入结果不确定、过期拒绝、用户明确取消；
   `internal_text_losses` 只按 §3.1 的语义解释，不按计数名称判断丢字。

### 11.10 冻结正式包闭环复验授权申请（待用户批准一次 UAC）

| 项 | 内容 |
| --- | --- |
| 源码 SHA | `488b6f5b7823c3265965809cdb7a312b50a8e268` |
| 正式包绝对路径 | `D:\Users\Smart Kevin\OneDrive\Desktop\EpoCanva\build\product\stage\EpoCanva-0.2.5-w3a-r3` |
| DLL SHA-256 | `911B63A3FB79BFC26A873E879A4A9B5661A4DE938121D1961AB78C62CC5E8294` |
| 注册身份/范围 | CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}`；en-US profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`；HKLM `Software\Classes\CLSID` 下本产品键 + `GUID_TFCAT_TIP_KEYBOARD` + `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`；仅此产品，不触碰其他输入法 |
| 应用 | 本轮新建的唯一空文件 + 本轮唯一新建 Notepad 进程（普通权限，`is_elevated=False`） |
| 输入/选择 | `alpha hel` → 真实候选窗口可见且高度至少两行 → 点击第 1 行（产品契约：第 0 行 RAW，第 1 行补全/拼写，非 RAW）→ 验证文档变化且 `alpha ` 前缀保持；不发送 Down/Enter，不硬编码 Down 次数；候选不存在或不足两行则停止 |
| 预计用时 | 2–3 分钟测试 + 清理；触发器/agent 硬截止 6 分钟 |
| 前台空闲要求 | 注册后等待前台空闲 ≥0.9 s 才由提权 relay 置前；每个按键前验证本轮 PID/HWND、文档子窗口焦点、en-US 布局、Shift/Ctrl/Alt/Caps、完整性级别；任何变化立即停止 |
| 停止/恢复/清理 | 失败或超时写 `stop.flag`，agent 不再输入并关闭本轮 Notepad/文件；finally 中 deactivate → unregister profile → `unregister_tip.ps1`，验证 COM、两个类别、TIP key、profile 全部 absent；恢复测试线程原 HKL |
| UAC 取消 | 立即停止，不自动重试、不沿用旧轮批准；不做任何注册/输入 |
| 已知限制 | 如果 Notepad 仍返回 `pbShow=FALSE` 且不绘制宿主候选，产品按契约**不会**自绘；本轮将在候选窗口缺失处停止并记录宿主限制，不发送任何选择输入。只有宿主返回 TRUE（或 `Show(TRUE)` 交回）且真实候选窗口可见时才点击第 1 行。若第 1 行结果不是 `held`，记录实际非 RAW 文本和前缀/焦点结果，不以硬编码文本判定失败 |

本申请对应本轮已经完成的 B 层证据（§11.8）：正式链路实现和回归已冻结，但正式包在真实
宿主中的 `pbShow` 行为、候选可见性和选择仍必须由这一次 UAC 轮次验证；不得用 trace
stage8 的自绘结果代替。
