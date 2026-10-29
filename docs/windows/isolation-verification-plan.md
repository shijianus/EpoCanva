# TSF 隔离验证方案与一次性授权请求（W3-A 更新）

本文件是 **C 层执行材料**，不是已执行记录。W3-A 只读确认了本机仍然没有已授权的专用
Windows 环境，因此没有注册、没有切换输入法、没有在真实应用中验证，也没有修改注册表。
下列注册/激活脚本在 W3-A 中被审查和加固，但**除只读 `status` 外均未运行**。

## 1. W3-A 只读环境结论（2026-10-29）

| 项 | 观测 | 依据 |
| --- | --- | --- |
| 主机 | Windows 11 Pro 10.0.26200 build 26200，x64，Lenovo 83DG 物理机 | `Win32_OperatingSystem` / `Win32_ComputerSystem` |
| 虚拟化 | `HypervisorPresent=False`；没有 VBoxManage / vmrun / qemu | 只读 CIM 与 `Get-Command` |
| Windows Sandbox / Hyper-V | **无法确认**：`Get-WindowsOptionalFeature` 需要提权，本会话非管理员；不能把提权失败写成“功能不存在” | 实际命令返回 “requires elevation” |
| 权限 | 当前用户不是本地管理员（`Admin=False`） | `WindowsPrincipal.IsInRole` |
| 授权 | **无**专用 VM/测试机；无快照；未注册 CLSID；未调用 `regsvr32`/`InstallLayoutOrTip`；未切换输入法 | 只读检查 + 下面的 `status` 输出 |
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

## 2. 需要的环境（一次性授权请求）

请授权一台**可回滚**的 Windows 11 x64 环境，满足：

1. **目标环境**：专用 VM/测试机（不是用户生产主机），本地管理员，已有可用快照或还原点；
   建议同一构建周期内不变。若使用 Windows Sandbox/Hyper-V，启用该功能本身需要提权并
   可能重启，必须单独授权；W3-A 没有启用任何虚拟化功能。
2. **已准备测试应用**：记事本（Win32 Edit）、一个实际用户应用（例如 Edge 或 VS Code，
   记录真实版本/控件类型，不按应用名称猜技术栈）；可选第三类应用。
3. **允许的系统变更**（仅这些）：
   - `HKLM\Software\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}\InprocServer32`
     （默认值 = 暂存产品 DLL 绝对路径，`ThreadingModel=Apartment`）；
   - 同 CLSID 的 `Implemented Categories\{34745C63-B2F0-4784-8B67-5E12C8701A31}`
     （`GUID_TFCAT_TIP_KEYBOARD`）；
   - `ITfInputProcessorProfiles::Register` + `AddLanguageProfile`（en-US，
     profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`）；
   - `ITfInputProcessorProfileMgr::ActivateProfile` / `DeactivateProfile`（当前会话）。
4. **提权/重启**：HKLM 与 profile 注册需要管理员；预计**不需要重启**。不要求关闭安全
   软件、不安装证书、不下载系统镜像、不修改用户默认输入法以外的系统设置。
5. **目录映射与网络**：把仓库快照（或 `build/product/stage` + 工具）复制进隔离机；
   产品运行期无网络 I/O，测试可离线。若用共享目录，产品数据只读；证据手工取回。
6. **回滚**：先做快照/还原点；`reg export` 记录 CLSID 与 TSF 分类现状；
   测试后用 `unregister_tip.ps1` + `tip_profile_helper.exe unregister`；异常时恢复快照。
7. **证据取回**：`capture_state.ps1` 前后快照、`status` 输出、注册表导出、日志、截图、
   `--diagnose` 输出；只使用固定测试字符串；不读取用户既有文档/凭据；不上传、不自动回传。

**未获授权前不执行任何注册、切换或系统级修改。** 本文件与工具的准备不构成授权。

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
| 候选窗口显示 | 记事本 | 真实 `BeginUIElement`；`pbShow` 语义；窗口位于真实 token 矩形 |
| 点击不抢焦点 | 记事本 | 点击候选后焦点仍在编辑框；`WM_MOUSEACTIVATE=MA_NOACTIVATE` |
| Enter/Escape/BackSpace | 记事本 | 正常与降级路径；不删除宿主既有文本；无迟到写入 |
| 快速连续输入/容量 | 记事本 | 1202 单元突发 → 全部宿主插入；同会话继续输入；无崩溃 |
| 移动/点击后旧候选 | 记事本 | 旧菜单选择不写入；同文本其他位置不被误换 |
| 真实 token 锚点 | 记事本 | `ReadTokenRect` 真实执行；窗口随真实 caret 而非合成 anchor |
| 宿主接管绘制 | 记事本/第二应用 | `pbShow=FALSE` 时不重复弹窗 |
| 停用/卸载 | 记事本 | 停用后不崩溃；`DllCanUnloadNow` 在 TSF 回调释放前为 S_FALSE |
| 注册移除/再启用 | 记事本 | 移除后应用恢复原输入法；再次注册可重新启用 |
| 设置生效时机 | 记事本 | 保存后下一次引擎启动/宿主重启生效；界面文案一致 |

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

- 正式注册 TIP 在真实应用/StrictTsf 前台的输入、候选窗口与点击。
- 真实 `ITfUIElementMgr` 的 Begin/Update/End 与宿主绘制协商。
- 真实 `GetTextExt`/`ReadTokenRect` 与实际 token 锚点。
- 真实 150/200% DPI、多显示器边界、屏幕边缘。
- 注册/激活/移除/再启用的端到端证据。
- 安装器与最终分发（本轮明确不做）。

## 8. 脚本清单

| 文件 | 用途 | W3-A 状态 |
| --- | --- | --- |
| `capture_state.ps1` | 只读记录 OS、模块版本、注册表快照、包身份与哈希 | 已解析检查；未在隔离机运行 |
| `register_tip.ps1` | 写入 CLSID/类别，带所有权标记、读回验证与失败回滚 | 已重写并解析检查；未运行 |
| `unregister_tip.ps1` | 只移除本产品内容，验证残留 | 已重写并解析检查；未运行 |
| `tip_profile_helper.cc` | 正式 CLSID/profile 的 register/unregister/activate/deactivate/status | 已构建；仅运行只读 `status` |
| `run_layer_c.ps1` | 矩阵模板：身份、`--diagnose`、矩阵打印 | 未运行 |
