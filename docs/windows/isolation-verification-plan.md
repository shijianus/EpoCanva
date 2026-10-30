# TSF 隔离验证方案与一次性授权请求（W3-A 更新）

本文件是 **C 层执行材料**。W3-A 最初只读确认本机没有已授权的专用 Windows 环境；随后
用户**明确拒绝 VM/第三方平台**，要求在本机隔离测试。2026-10-30 已在本机执行临时
HKLM 注册/激活/移除并在每轮后验证无残留；结果见
`docs/reports/W3-A-isolation-registration.md` §11。**trace 构建曾跑通一次候选闭环**：
`alpha hel` → 真实引擎 8 项候选 → Down×2 选择 `held` → Enter → `alpha held`，焦点保持，
候选窗口关闭。**但该 trace 构建在 `pbShow=FALSE` 时自绘，违反 TSF UILess 契约**，不能作为
正式链路证据。修正后的正式链路（严格遵从 `pbShow`、注册
`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`、FALSE 后首次 Update、无选择 `S_FALSE`、Show/IsShown）
已通过 B 层完整回归（149 项：148 passed、0 failed、1 skipped）。冻结正式包
`build/product/stage/EpoCanva-0.2.5-w3a-r3/`，源码 `488b6f5`，DLL SHA-256
`911B63A3...E8294`，manifest 21/21；用正式包的闭环复验尚未执行，需一次明确 UAC。
旧的 `EpoCanva-0.2.5-w3a-r2`（`1EB48D19...92AA`）是待验证构建，不是正式链路通过。

> **状态（2026-10-30）**：§2 的三选一 VM/Sandbox 请求不再使用；VM 已关闭，未再启动。
> 本机注册/移除已验证无残留。trace 构建闭环通过不等于正式包通过；§4 矩阵在正式包
> 最小闭环复验通过后再执行。

## 1. W3-A 只读环境结论（2026-10-29）

| 项 | 观测 | 依据 |
| --- | --- | --- |
| 主机 | Windows 11 Pro 10.0.26200 build 26200，x64，Lenovo 83DG 物理机 | `Win32_OperatingSystem` / `Win32_ComputerSystem` |
| 虚拟化 | 固件已启用 VT-x（`VirtualizationFirmwareEnabled=True`、SLAT=True、DEP=True）；**hypervisor 未运行**（`CsHypervisorPresent=False`，`systeminfo` 列出硬件需求）；VMware Workstation 17.6.4 已安装，`vmrun`/`vmcli` 在安装目录 | `Win32_Processor` / `Get-ComputerInfo` / 文件只读 |
| Windows Sandbox / Hyper-V | Sandbox **Enabled**（`Containers-DisposableClientVM` InstallState=1，`WindowsSandbox.exe` 存在）；Hyper-V All/Hypervisor/Services/Management-PowerShell Enabled；`HypervisorPlatform` Disabled。当前 hypervisor 未运行，启动 Sandbox 需要启用 hypervisor 启动项并重启（未执行） | `Win32_OptionalFeature`（无需提权） |
| 权限 | 用户 `Smart Kevin` 在 `Administrators` 组，但当前进程是过滤令牌（未提升）；一次 UAC 提权请求被取消 | `net localgroup` + `WindowsIdentity` |
| 授权 | **无**已确认的专用 VM；未注册 CLSID；未调用 `regsvr32`/`InstallLayoutOrTip`；未切换输入法；未启动 VM/Sandbox | 只读检查 + 下面的 `status` 输出 |
| 现有 VM | VMware inventory 6 条、3 个本地 vmx：两个 Win11 `MyWindows11 US-Program`（一个无快照、一个有 2 个 2026-08-04 `.vmsn` 但 `.vmsd` 加密、`vmrun listSnapshots` 无输出）、一个 Ubuntu；一个 `vmware-vmx` 自 2026-10-24 运行而 `vmrun list` 为空，状态不一致，未触碰 | 文件/注册表只读 |
| 本地安装介质 | `Win11_24H2_Chinese_Simplified_x64.iso`、`Windows10.iso`、`windows.iso`（VMware Tools）在 VMware Workstation 目录；无需下载 | 目录只读 |
| 产品身份 | CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}`；en-US profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`；类别 `{34745C63-B2F0-4784-8B67-5E12C8701A31}` | `platforms/windows/tsf/identifiers.{h,cc}` |
| 产品 DLL | x64 构建；正式 DLL 只导出 `DllGetClassObject`/`DllCanUnloadNow` | 构建产物 + 脚本的 PE/导出检查 |
| 现有证据层 | A（纯逻辑）、B（真实 TSF TextStore + 直接 sink + 真实编辑会话） | CTest 结果 |
| 不能替代 C 层 | 直接 sink 变体不创建真实 `ITfUIElementMgr`，没有真实前台应用/文本矩形 | 代码与测试矩阵 |

W3-A 在**未授权的本机**上执行的唯一注册相关操作是只读 `status`：

```
build/w3a-isolation-tools/isolation-tools/Release/epocanva_tip_profile_helper.exe status
product_clsid   {B440F702-63E4-4D47-BCC4-074899ABA836}
product_profile {7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}
com_registration HKLM     absent
com_registration HKCU     absent
CoCreateInstance(profile mgr)    0x00000000
profile_present          no
profile_enabled          no
profile_active_keyboard  no
GetProfile                       0x80004005
GetActiveProfile                 0x00000000
```

该命令不写注册表、不注册 profile、不切换输入法；它证明本机产品状态为“未注册”。

### 1.1 2026-10-30 本机测试结果（临时 HKLM 注册，已清理）

- `register_tip.ps1 -Hive HKLM` + helper `register`：`Register CLSID 0x00000000`、
  `AddLanguageProfile 0x00000000`；profile 数秒后 present/enabled。profile 可见性是异步的，
  必须轮询，不能立即判失败。
- 全新 Notepad：TIP DLL `epocanva_tsf.dll` 与 `rime.dll` 均加载；en-US HKL 切换成功；
  `RichEditD2DPT` 文档焦点正确；唯一空文件中输入 `alpha hel` 正确。
- **trace 构建最小闭环（诊断，非正式）**：候选窗口可见（8 项，RAW `hel` + COMP `held` 等）；
  Down 两次选择第 1 页，Enter 提交；文档变为 `alpha held`，焦点仍在原文档，候选关闭；
  `agent_passed=True`、`cleanup_verified=True`。该轮的候选窗口来自 stage8 在
  `pbShow=FALSE` 时的自绘，**不是**正式契约允许的行为；Notepad 返回 FALSE 且未自绘是
  宿主兼容限制，需要用修正后的正式包重新观测。
- **正式 -r2 包未通过**：DLL `1EB48D19...92AA`、manifest 21/21，但含 Notepad 进程名
  白名单且缺 UIElement 类别注册；两次 UAC 均被取消（退出码 7），没有注册/输入。该包
  只作为对照，不作为正式链路结论。
- **冻结 -r3 包待复验**：源码 `488b6f5`，DLL `911B63A3...E8294`，manifest 21/21，
  trace OFF、导出仅正式接口；尚未在真实宿主中注册/输入。复验申请见 W3-A 报告 §11.10。
- 清理：每轮 deactivate/unregister + `unregister_tip.ps1`，`status` 最终 HKLM/HKCU
  absent、profile absent、active no；本轮测试 Notepad/文件无遗留。
- 证据：`build/w3a-c-local/evidence6/`（gitignored）；早期失败轮次在 `evidence5` 及更早。
- 下一步：用正式包复验最小闭环（一次 UAC）；通过前不执行 §4 矩阵。任何按键前必须验证
  前台与文档焦点，选择前再次验证，测试后立即移除并验证残留。

## 2. 需要的环境（三选一授权请求；已被 2026-10-30 本机测试取代，不再使用）

请明确授权以下一种隔离环境。共同要求：Windows 11 x64、真实 TSF/桌面输入、记事本 +
第二真实应用；注册/激活/移除**只在隔离环境内**；产品数据只读、证据目录唯一可写；
不映射仓库、OneDrive、桌面或用户目录；固定测试字符串；不上传、不自动回传。

1. **方案 A（推荐，无需改宿主功能/重启）**：用本机 `Win11_24H2_Chinese_Simplified_x64.iso`
   新建 VMware 专用 VM（`D:\EpoCanvaIsolation\W3A-Win11`，4 vCPU/8 GiB/40 GiB 薄置备，
   UEFI + vTPM 测试口令，网卡断开），安装 VMware Tools，关机后快照 `w3a-clean`。
   guest 内 `C:\w3a\input` 为只读 HGFS 共享、`C:\w3a\output` 为唯一可写 HGFS 共享
   （`vmcli HGFS SetWriteAccess`）；`vmcli MKS` 键盘/截图 + `vmcli Guest run` 运行 guest 内
   PowerShell（UIA/SendInput/GetGUIThreadInfo）；`vmcli Guest copyFrom` 或输出共享取回证据；
   测试后 `Snapshot Revert w3a-clean`，必要时删除 VM。
2. **方案 B（使用你指定的现有 Win11 VM）**：需要 VM 名称/路径、确认它可作为一次性测试
   环境、VM 加密口令与 guest 管理员口令；运行中的 VM 先由你关闭。开始前确认/新建快照，
   没有可用快照就不开始；其余同 A。该 VM 可能含个人数据，未确认前不使用。
3. **方案 C（需要提权 + 重启）**：`bcdedit /set {current} hypervisorlaunchtype auto`
   （管理员）+ 重启；若 VMware 需要共存再启用 `HypervisorPlatform`（再次重启）。
   Sandbox 配置：`Networking=Disable`、`ClipboardRedirection=Disable`、vGPU/Audio/Video/
   Printer 关闭；`D:\EpoCanvaIsolation\input` 只读、`...\output` 可写；
   `LogonCommand` 在 Sandbox 内自驱，关闭即销毁。只有一个虚拟显示，DPI/多屏覆盖有限。

**允许的系统变更（仅隔离环境内）**：
- `HKLM\Software\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}\InprocServer32`
  （默认值 = 隔离环境内暂存 DLL 的绝对路径，`ThreadingModel=Apartment`）；
- 同 CLSID 的 `Implemented Categories\{34745C63-B2F0-4784-8B67-5E12C8701A31}`
  （`GUID_TFCAT_TIP_KEYBOARD`）；
- `ITfInputProcessorProfiles::Register` + `AddLanguageProfile`（en-US，
  profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`）；
- `ITfInputProcessorProfileMgr::ActivateProfile` / `DeactivateProfile`（隔离会话内）。

**宿主侧不注册、不切换输入法；方案 A/B 不改宿主功能、不重启。** 未获授权前不启动
VM/Sandbox、不注册、不切换。本文件与工具的准备不构成授权。

## 3. 工具与精确命令（获授权后）

### 3.1 构建与身份

```
powershell -File tools/product/build_product.ps1 -SkipTests
powershell -File tools/product/stage_product.ps1 -Replace
<stage>\EpoCanva.exe --diagnose <evidence>\diagnose.txt
```

构建隔离工具（不进入产品包）：

```
cmake -S platforms/windows/engine_bridge -B build/isolation-tools -G "Visual Studio 17 2022" -A x64 ^
      -DEPOCANVA_BUILD_PRODUCT=ON -DEPOCANVA_BUILD_ISOLATION_TOOLS=ON ^
      -DEPOCANVA_RIME_DLL=<rime.dll> -DEPOCANVA_ENGLISH_FIXTURE_DIR=<fixtures> ^
      -DEPOCANVA_SPELLING_FIXTURE_DIR=<spelling> -DEPOCANVA_PRODUCT_LEXICON=<lexicon>
cmake --build build/isolation-tools --config Release --target epocanva_tip_profile_helper
```

`epocanva_tip_profile_helper.exe` 只做 profile 操作；它不写 CLSID/类别键，也不宣称
“profile 注册成功 = 已在应用激活”。`status` 分别报告 COM 注册、profile 存在和当前
键盘 TIP 激活状态。

### 3.2 注册、检查、移除

```
powershell -File tools/product/isolation/capture_state.ps1 -ReportPath <evidence>\before.txt -DllPath <stage>\epocanva_tsf.dll
powershell -File tools/product/isolation/register_tip.ps1 -DllPath <stage>\epocanva_tsf.dll -AuthorizedIsolationEnvironment -EvidencePath <evidence>\registration.json
<helper> register
<helper> status
<helper> activate
... C 层矩阵 ...
<helper> deactivate
<helper> unregister
powershell -File tools/product/isolation/unregister_tip.ps1 -AuthorizedIsolationEnvironment -EvidencePath <evidence>\removal.json
powershell -File tools/product/isolation/capture_state.ps1 -ReportPath <evidence>\after.txt
```

`register_tip.ps1` 会：

- 拒绝未授权开关、非 x64 DLL、非管理员 HKLM；
- 读取 PE 头验证 `0x8664`，若 `dumpbin` 可用则检查 `DllGetClassObject`/`DllCanUnloadNow`
  （不要求 `DllRegisterServer` 存在）；
- 遇到属于其他产品的 CLSID（无 `EpoCanvaProduct` 标记且路径不是 `epocanva_tsf.dll`）时拒绝；
- 写入后逐项读回 `InprocServer32`、`ThreadingModel`、标记和类别；任一不符即回滚；
- 输出 JSON 证据（DLL 路径/SHA-256、写前状态、每个操作的返回值）。

`unregister_tip.ps1` 只删除带本产品标记或指向 `epocanva_tsf.dll` 的内容；移除后重新读取
并断言 `InprocServer32`、类别和标记都没有残留；不触碰其他输入法或用户设置。

## 4. 测试矩阵（C 层，未执行）

| 场景 | 应用 | 断言 |
| --- | --- | --- |
| 正式 TIP 激活 | 记事本 | 输入固定英文 token，Space 提交，文档精确；无网络 I/O |
| StrictTsf 前台 | 记事本/第二应用 | `ContextObserver` 以 StrictTsf 绑定真实前台焦点；无 Lab fallback |
| 候选窗口显示 | 记事本 | 真实 `BeginUIElement`；TIP 初值 `pbShow=TRUE`；宿主 FALSE 时不自绘、FALSE 后立即首次全量 Update；宿主 TRUE 时窗口位于真实 token 矩形；宿主不绘制时记录限制 |
| 点击不抢焦点 | 记事本 | 点击候选后焦点仍在编辑框；`WM_MOUSEACTIVATE=MA_NOACTIVATE` |
| Enter/Escape/BackSpace | 记事本 | 正常与降级路径；不删除宿主既有文本；无迟到写入；宿主隐藏 UI 时方向键/Enter 不进产品菜单 |
| 快速连续输入/容量 | 记事本 | 1202 单元突发 → 全部宿主插入；同会话继续输入；无崩溃 |
| 移动/点击后旧候选 | 记事本 | 旧菜单选择不写入；同文本其他位置不被误换；非折叠选区拒绝 |
| 真实 token 锚点 | 记事本 | `ReadTokenRect` 真实执行；锚点只等于 token 起点+宿主单元数；快照 caret 落后 1 时仍可选 |
| 宿主接管绘制 | 记事本/第二应用 | `pbShow=FALSE` 时不重复弹窗；`Show(FALSE)` 后不重开；`Show(TRUE)` 才交回 TIP |
| 停用/卸载 | 记事本 | 停用后不崩溃；`DllCanUnloadNow` 在 TSF 回调释放前为 S_FALSE |
| 注册移除/再启用 | 记事本 | 移除后应用恢复原输入法；再次注册可重新启用 |
| 设置生效时机 | 记事本 | 保存后下一次引擎启动/宿主重启生效；界面文案一致 |

### 4.0 本轮测试编排的会话握手（正式包复验必须使用）

- 每次 UAC 授权只对应一个 `session_id`（GUID）；注册完成、agent 就绪、测试开始、
  截止时间、清理完成五个标志都写入并校验同一个 `session_id`。任何标志缺失或属于旧
  session → 该轮无效，不发送任何按键。
- 截断时间：触发器和普通权限 agent 各自持有同一个绝对截止时间；注册后、agent 启动前、
  每个按键前、每次选择前都检查。超过截止时间或检测到墙钟相对单调时钟跳变 >30 s
  （休眠/唤醒）→ 立即写 `stop.flag`，agent 停止输入并标记 `invalid_round=true`；不继续
  输入、不复用旧授权。
- 注册/激活只由提权 helper 执行；agent 与 Notepad 始终普通权限（`is_elevated=False`）。
  agent 创建唯一 Notepad 与唯一空文件；输入前验证本轮 PID/HWND、文档子窗口焦点、en-US
  输入布局、Shift/Ctrl/Alt/CapsLock、前台根窗口。任何条件变化立即停止；不强制释放用户
  物理按下的修饰键。
- 注册前先确认 stage 是**完整产品布局**（`EpoCanva.exe`、`epocanva_tsf.dll`、`rime.dll`、
  `package.manifest`、`data/default.yaml`、`data/epocanva_english.schema.yaml`、
  `data/lexicon/epocanva_en_alpha.tsv`），并运行 stage 自带
  `EpoCanva.exe --diagnose <evidence>\diagnose.txt`；要求 `package=ready`、
  `manifest=ok`、`manifest_files=21`、`manifest_failures=0`。不允许只复制 DLL 后注册。
- 清理：deactivate → unregister profile → `unregister_tip.ps1`；验证 COM、两个类别
  （`GUID_TFCAT_TIP_KEYBOARD` 与 `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`）、TIP key 和
  profile 全部 absent，并把结果连同 session ID 写入 `cleanup-done.flag`。
- UAC 取消 → 触发器退出 7，停止；不自动重试、不沿用旧轮批准。

### 4.1 证据最小集（每次显式选择）

1. 输入前文本（固定测试串）；
2. 真实候选菜单（截图 + `status`/引擎页数）；
3. 显式选择（方向键/Enter 或点击）；
4. 替换后文本；
5. 焦点仍在原编辑控件（`GetGUIThreadInfo`/截图）。

“全部按键直通、原文没丢”只证明输入保真，**不能**单独算输入法功能通过；必须证明真实
候选确实改变了目标 token，且没有改到其他地方。

## 5. DPI 与多显示器（未执行）

隔离环境支持时验证 100%/150%/200%、屏幕边缘、小工作区和多显示器。合成 anchor、假
manager 或修改窗口尺寸参数不能冒充真实多显示器/DPI 结果；W3-A 的 B 层布局测试只报告为
“受控布局”，不列为 C 层通过。

## 6. 证据与隐私

- 只用固定测试字符串（如 `hello world`、`user@example.com`、`C:\temp\file.txt`）。
- 生产日志不记录按键、组合缓冲、提交文本、剪贴板、字段名或应用标识。
- dump/PDB/测试 DLL/临时注册工具只留在隔离机的开发测试目录；不进入产品包、不自动上传。
- 失败时记录：提交 SHA、包版本、OS/故障模块版本、异常码/地址/读写类型、线程栈、
  产品计数器、复现命令；不记录用户文本。

## 7. 仍未执行（设备/授权缺失）

- 修正后正式注册 TIP 在真实应用/StrictTsf 前台的输入、候选窗口与点击。
- 真实 `ITfUIElementMgr` 的 Begin/Update/End 与宿主绘制协商；`pbShow=FALSE` 后首次
  Update、`Show(FALSE)/IsShown`、无选择 `S_FALSE` 的真实宿主行为。
- 真实 `GetTextExt`/`ReadTokenRect` 与实际 token 锚点；快照 caret 落后 1 时的选择。
- 真实 150/200% DPI、多显示器边界、屏幕边缘。
- 注册/激活/移除/再启用的端到端证据。
- 安装器与最终分发（本轮明确不做）。

## 8. 脚本清单

| 文件 | 用途 | W3-A 状态 |
| --- | --- | --- |
| `capture_state.ps1` | 只读记录 OS、模块版本、注册表快照、包身份与哈希 | 已解析检查；未在隔离机运行 |
| `register_tip.ps1` | 写入 CLSID + 键盘 + UIElement 两个类别，带所有权标记、读回验证与失败回滚 | 已重写并解析检查；修正后未运行 |
| `unregister_tip.ps1` | 只移除本产品内容（含两个类别），验证残留 | 已重写并解析检查；修正后未运行 |
| `tip_profile_helper.cc` | 正式 CLSID/profile 的 register/unregister/activate/deactivate/status | 已构建；仅运行只读 `status` |
| `run_layer_c.ps1` | 矩阵模板：身份、`--diagnose`、矩阵打印 | 未运行 |
| `build/w3a-c-local/trigger.ps1` 等 | 本机单轮 session/截止时间编排、普通权限 agent、DWIN trace 监听；gitignored | 已补齐 session 握手并解析检查；待授权后运行一次 |
