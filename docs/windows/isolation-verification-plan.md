# TSF 隔离验证方案与一次性授权请求（W2-B-R）

本文件是 **W3 的执行材料**，不是已执行记录。W2-B-R 期间只读确认了本机没有已授权的专用
Windows 环境，因此没有注册、没有切换输入法、没有在真实应用中验证，也没有修改注册表。
下列脚本**未运行**，不得在用户主系统上运行。

## 1. 只读环境结论（2026-10-29）

| 项 | 观测 |
| --- | --- |
| 主机 | Windows 11 x64 build 26200（26200.9711）；物理机；`HypervisorPresent=False` |
| 授权 | **无**专用 VM/测试机；未注册 CLSID；未调用 `regsvr32`/`InstallLayoutOrTip`；未切换输入法 |
| 现有证据层 | A（纯类型）、B（受控 ITextStoreACP + 直接 sink） |
| 不能替代 C 层 | 直接 sink 变体不创建真实 `ITfUIElementMgr`，没有真实前台焦点/应用/文本矩形 |

## 2. 需要的环境（一次性授权请求）

请授权一台**可回滚**的 Windows 11 x64 环境，满足：

1. 本地管理员；可在快照/还原点之间回滚；不是用户的生产主机。
2. 已安装固定测试应用：记事本（Win32 编辑框）、Microsoft Edge（浏览器文本框）、
   VS Code（Electron/TSF 编辑器）各一个固定版本。
3. 允许临时写入以下注册项（并在测试后删除）：
   - `HKLM\Software\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}`
     - `InprocServer32` 默认值 = 产品 DLL 绝对路径；`ThreadingModel = Apartment`
     - `InprocServer32\ProgID` 可选，不需要
   - `HKLM\Software\Classes\CLSID\{B440F702-...}\Implemented Categories\{34745C63-B2F0-4784-8B67-5E12C8701A31}`
     （`GUID_TFCAT_TIP_KEYBOARD`）
   - 通过 `ITfInputProcessorProfiles::Register` + `AddLanguageProfile` 建立 en-US 键盘
     profile；用 `ITfInputProcessorProfileMgr::ActivateProfile` 启用/停用。
4. 允许把输入法切换到 EpoCanva、切回原输入法；允许为测试创建/删除一个本地用户
   （可选，但建议）。
5. 不要求联网。EpoCanva 运行期无网络 I/O；隔离机可保持离线。
6. 提供回滚点名称、操作时间窗、执行人和证据存放位置。

**未获授权前不执行任何注册、切换或系统级修改。**

## 3. 最小执行步骤（获授权后）

1. 快照/还原点；记录 `reg export` 的 CLSID 与 TSF 分类现状；记录 `Get-WinEvent` 基线。
2. 用隔离机上的 `build_product.ps1` 从**待验证提交**构建 Release 产品，运行
   `stage_product.ps1 -Replace`，用 `EpoCanva.exe --diagnose` 记录包身份。
3. 运行 `tools/product/isolation/register_tip.ps1`（需要显式授权开关与管理員）。
4. 运行 `tools/product/isolation/tip_profile_helper.exe register|activate`（W3 构建，
   不在产品包内），或按脚本文本手动在“设置 → 时间和语言 → 语言和区域”中添加键盘。
5. 按 §4 矩阵执行 `run_layer_c.ps1`，保存日志、注册表快照、截图和 `--diagnose` 输出。
6. 运行 `unregister_tip.ps1` 与 `tip_profile_helper.exe unregister`；删除 profile；
   对比注册表快照；确认输入法列表恢复；必要时回滚快照。
7. 只在隔离机保留 dump/PDB/日志；不上传、不进入产品包。

## 4. 测试矩阵（C 层）

| 场景 | 应用 | 断言 |
| --- | --- | --- |
| 正式 TIP 激活 | 记事本 | 输入英文 token，Space 提交，文档精确；无网络 I/O |
| StrictTsf 前台 | 记事本/Edge | `ContextObserver` 以 StrictTsf 绑定真实前台焦点；无 Lab fallback |
| 候选窗口显示 | 记事本 | 真实 `ITfUIElementMgr::BeginUIElement`；`pbShow` 语义；窗口位于真实 `GetTextExt` 矩形 |
| 点击不抢焦点 | 记事本 | 点击候选行后焦点仍在编辑框；`WM_MOUSEACTIVATE=MA_NOACTIVATE` |
| Enter/Escape/BackSpace | 记事本 | 正常与降级路径；不删除宿主既有文本；无迟到写入 |
| 容量边界同会话 | 记事本 | 1202 单元突发 → 1024 块应用 → 继续输入 → 引擎恢复；无崩溃 |
| 停用/卸载 | 记事本 | 停用后不崩溃；`DllCanUnloadNow` 在 TSF 释放回调前为 S_FALSE |
| DPI | 记事本 | 100/150/200%；窗口在光标附近，不溢出工作区 |
| 多显示器/边缘 | 记事本 | 负坐标/小工作区；窗口翻转；不抢焦点 |
| 真实矩形读取 | 记事本 | `ReadCompositionRect` 真实执行（B 层未执行，必须保留为未验证直到此处） |

## 5. 证据与隐私

- 只用固定测试字符串（如 `hello world`、`user@example.com`、`C:\temp\file.txt`）。
- 生产日志不记录按键、组合缓冲、提交文本、剪贴板、字段名或应用标识。
- dump/PDB/测试 DLL 只留在隔离机的开发测试目录；不进入产品包、不自动上传。
- 失败时记录：提交 SHA、包版本、OS/故障模块版本、异常码/地址/读写类型、线程栈、
  产品计数器和复现命令。

## 6. 仍未执行（设备/授权缺失）

- 正式注册的 TIP 在真实应用/StrictTsf 前台下的输入与候选窗口。
- 真实 `ITfUIElementMgr` 的 Begin/Update/End 与宿主协商绘制。
- 真实 `GetTextExt`/`ReadCompositionRect`；当前 B 层用合成 anchor 或用例不执行该路径。
- 真实 150/200% DPI 像素外观、多显示器边界、屏幕边缘。
- 注册/卸载/安装器本身（W3，不在本轮）。

## 7. 脚本清单（均已写好，未运行）

| 文件 | 用途 |
| --- | --- |
| `capture_state.ps1` | 只读记录 OS、模块版本、注册表快照、包身份与哈希 |
| `register_tip.ps1` | 写入 CLSID/分类；必须显式 `-AuthorizedIsolationEnvironment` 且管理员 |
| `unregister_tip.ps1` | 删除上述键；必须显式授权开关 |
| `tip_profile_helper.cc` | 用 `ITfInputProcessorProfiles` 注册/启用 profile 的最小 W3 辅助工具源码（本轮不构建、不运行） |
| `run_layer_c.ps1` | 模板：构建身份、`--diagnose`、按矩阵运行固定测试并收集证据（需人工确认） |
