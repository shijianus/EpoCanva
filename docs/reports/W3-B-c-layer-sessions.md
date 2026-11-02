# W3-B 报告增补 — C 层六轮真实会话（2026-11-02）

签发日期：2026-11-02
起点 HEAD：`5c0f11b`（工作区另含 4 个未提交的隔离工具修复，见 §4）
冻结包：`build/product/stage/EpoCanva-0.2.6-w3b/`（21 载荷文件；
`epocanva_tsf.dll` SHA-256 `5BE01155EFF508ADB3DE4BE09C9447111233DDA841E30BC8BC383F17962AA604`，
与 W3-B 报告 §8 冻结值一致，执行前实测复核）
授权：负责人预先批准（"UAC 弹窗直接同意"），每轮一次 UAC，共 **6 次**（W3-B 预算 5–6 次已用尽）
执行者：自动化协作者 + 负责人现场配合（UAC 批准、第 6 轮 Notepad 点击）

---

## 1. 六轮会话总表

全部六轮均使用未启用 trace 的正式包（`-Formal`）、同一运行副本纪律
（`stage_run_copy.ps1` 固化 + 逐文件 SHA-256）、每轮前 3 次无提权预检全绿。

| 轮 | session_id（前 8 位） | 预检 | UAC | 注册/激活 | Agent 结果 | 清理残留 | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `56cc478d` | 3/3 绿 | 批准 | S_OK | 中止：`[W3ANative]` 类型不存在（工具缺陷 1） | 逐项读回为零 | 环境证据有效，输入未执行 |
| 2 | `17144c15` | 3/3 绿 | 批准 | S_OK | 中止：wall clock jumped（工具缺陷 2 假阳性） | 逐项读回为零 | **rime.dll 已被真实宿主加载**（`rime_dll_loaded_before_typing=True`）；布局成功切 en-US（`0x08040804→0x04090409`） |
| 3 | `7dec93ea` | 3/3 绿 | 批准 | S_OK | 中止：notepad is not foreground（自动尝试预算仅 1.3 秒） | 逐项读回为零 | 环境证据有效 |
| 4 | `1fc129a7` | 3/3 绿 | 批准 | S_OK | 中止：notepad is not foreground（16 次自动尝试 ×16 秒全败） | 逐项读回为零 | 前台锁被操作员终端窗口（`CASCADIA_HOSTING_WINDOW_CLASS`，hwnd 395342）持有 |
| 5 | `e73c2bd9` | 3/3 绿 | 批准 | S_OK | 中止：notepad is not foreground（16 次尝试 + 90 秒操作员等待，未有点击） | 逐项读回为零 | 工具等待逻辑按设计工作 |
| 6 | `46051a7d` | 3/3 绿 | 批准 | S_OK | **打字已执行**：9/9 键入、文档逐字落地 `alpha hel`、标题带修改标记；10 秒轮询无候选窗口 → 按契约中止 | 逐项读回为零 | **真实 TSF 键入打穿**；引擎冷启动与打字节奏竞态（见 §3.3） |

每轮证据目录：`build/w3a-c-local/evidence-<完整 session_id>/`（agent-result.json、
agent.log、registration.json、removal.json、elevated-summary.txt、status-*.txt、
第 6 轮含 after-typing-1s.png / before-selection.png 截图）。
预检记录：`build/w3a-c-local/preflight/session-*/preflight-summary.json`（6 组）。
运行副本：`build/w3a-c-local/run-20261102-{034812191,035235629,040218153,040515754,040834013,041409867}`。

## 2. 已在真实环境证明的事实（此前从未证明过）

1. **真实注册/激活**：HKLM `Software\Classes\CLSID\{B440F702-…}` 下的 `InprocServer32`
   （指向冻结包绝对路径）、`EpoCanvaProduct` 标记、keyboard 与 UIElement-enabled 两个实现类别，
   加 TSF profile 注册（`Register CLSID 0x00000000`、`AddLanguageProfile en-US 0x00000000`）
   与 `ActivateProfile 0x00000000`——**六轮全部 S_OK**，注册后 `profile_present=yes`、
   `profile_enabled=yes`。
2. **TIP 真实加载进宿主**：第 2、6 轮实测 `rime_dll_loaded_before_typing=True`——
   注册后的产品 DLL 被 Windows 11 记事本（`RichEditD2DPT`）作为 TIP 真实加载。
3. **真实键入链路**：第 6 轮 9 个键经真实 TSF 管线进入文档，逐字落地 `alpha hel`，
   标题出现修改标记；en-US 键盘布局切换成功。
4. **真实反注册与零残留**：六轮 finally 路径 deactivate → profile unregister →
   COM 反注册 → 逐项读回，`post_hklm_com=False`、`post_hkcu_com=False`、
   `post_hklm_tip/post_hkcu_tip=False`、两类别 absent、`profile_present=no`、
   `profile_active_keyboard=no`，`cleanup_verified=True` **六次全部成立**。
   这相当于把 DoD 7 的"卸载后残留为零"在真实注册表上验证了六遍。
5. **会话握手纪律有效**：每次中止都发生在安全门禁上（类型缺失、时钟、前台），
   没有任何一次盲发输入；清理路径从未被跳过。

## 3. 本轮新发现（均为实测，附证据）

### 3.1 工具缺陷 1：`W3ANative` 编译失败被静默吞掉（已修复）

`c_layer_agent.ps1` 内嵌 C# 块重复定义 `CloseHandle`（CS0111），`Add-Type` 编译失败；
脚本 `$ErrorActionPreference='Continue'` 把错误吞掉，类型从未定义。预检走独立的
`LayerCPreflightNative` 所以一直绿，而正式轮在 `CreateProcess` 处得到
`Unable to find type [W3ANative]`（第 1 轮 `formal_win32_error=203`、launcher_pid=0）。
**这正是 W3-A 报告 §11.11 记录的"9 秒后放弃、无按键"的根因**——正式输入轮次从 W3-A
起就从未能启动过 Notepad。修复：删除重复定义，保留带 `SetLastError=true` 的版本；
探针实测编译通过（45 个方法，含 `CreateProcess`）。

### 3.2 工具缺陷 2：`[Environment]::TickCount64` 在 PS 5.1 不存在（已修复）

Windows PowerShell 5.1 运行在 .NET Framework 4.8 上，`[Environment]::TickCount64`
（.NET Core 3.0 才加入）静默求值为 `$null`（探针实测 `expression_is_null=True`），
于是 `$mono` 恒为 0，"时钟跳变检测"变成"轮次开始 30 秒后必然触发"的假阳性
（第 2 轮中止原因 `wall clock jumped`，而事件日志无任何睡眠/唤醒/校时记录）。
trigger 与 agent 同受影响。修复：两文件改用 kernel32 `GetTickCount64` P/Invoke
（`EpoLayerCTick`），语义不变（休眠不前进）；41 秒漂移探针实测偏差 0.01 秒。

### 3.3 引擎冷启动与打字节奏竞态（产品行为符合设计；下一轮工具已适配）

第 6 轮时间线：TIP 激活 → 记事本加载 DLL → `%LOCALAPPDATA%\EpoCanva\engine\user.yaml`
的 `schema_access_time` 实测 **04:17:40**（会话建立、schema 选择完成）→ agent 于
**04:17:41** 开始以约 9 键/秒打字，1.2 秒内 9 键全部落地 → 全部键都在就绪转换完成前
进入宿主，按 W2-C 保真设计逐字 literal 直通（截图确认无组合下划线）→ 组合从未存在，
候选窗口无从出现 → 10 秒轮询无果，按契约中止。

这不是产品缺陷：literal 优先、建议有界可丢弃正是 W2-C 的验收行为。它是 **W3-A 冻结的
"窗口就绪即打字"契约与 W2-B 异步引擎启动之间的节奏错配**，此前不可见是因为工具缺陷
使正式轮从未走到过打字。已修复 agent（维护工具，非产品）：键间 400ms 停顿 +
候选轮询 10 秒→25 秒，断言集合不变。

### 3.4 Windows 前台锁需要操作员配合（新的会话前提）

UAC 安全桌面往返后，最后输入事件所属窗口（操作员的终端，
`CASCADIA_HOSTING_WINDOW_CLASS`）持有前台权；隐藏的后台 agent 连续
`SwitchToThisWindow` + `AttachThreadInput` 16 次也无法夺取。第 6 轮证明**操作员点击
Notepad 窗口一次**是唯一可靠授权路径。agent 已扩展为：16 次自动尝试 +
最长 180 秒的操作员点击等待轮询；判定契约不变（前台不是 Notepad 就拒绝打字）。

### 3.5 Windows 11 记事本标签恢复（R3 的实证）

第 6 轮截图显示新记事本窗口恢复了此前各轮的 `w3a-preflight`/`w3a-test-*` 标签页十余个。
agent 的唯一窗口身份校验（新窗口、新进程、标题含唯一叶名）仍然全部通过，本轮未受实际
影响；但后续轮次解释截图与窗口枚举时必须把"标签恢复"计入。

## 4. 文件变更（全部为维护中测试工具，未提交；AGENTS §9）

- `tools/product/isolation/layer_c/c_layer_agent.ps1`：
  修复 1（删除重复 `CloseHandle`）、修复 2（`EpoLayerCTick` 基线与两处 `$mono`）、
  前台获取扩展（16 次自动重试 + 180 秒操作员等待，`foreground_attempts`/
  `foreground_operator_wait*` 进证据）、节奏化打字（键间 400ms）与候选轮询 25 秒。
- `tools/product/isolation/layer_c/trigger.ps1`：修复 2（`EpoLayerCTick`）。
- 诊断探针（临时，位于 `build/`）：`probe-addtype.ps1`、`probe-tick64.ps1`、
  `probe-tickdrift.ps1`、`probe-parse.ps1`、`probe-timeevents.ps1`。

产品代码、`third_party/`、夹具、引擎：**一个文件都没改**。

## 5. 实际执行的命令与退出码（要点）

- `stage_run_copy.ps1` ×6 → exit 0（运行副本 + run-identity.json）
- `run_preflight_3.ps1` ×6 组 ×3 次 → 全部 `preflight_run=i ok=True`（exit 0）
- `trigger.ps1 -Elevate -Formal -StageDir <冻结包>` ×6 →
  第 1–5 轮 exit 8（invalid/test 标志缺失），第 6 轮 exit 3（agent 按契约中止，
  `agent_invalid_round=False`）；六轮 `preflight_ready=True`、`registration_ready=True`、
  `cleanup_seen=True`、`cleanup_verified=True`。
- 事件日志探针：无睡眠/唤醒（Kernel-Power 42）、无唤醒（Power-Troubleshooter 1）、
  无系统时钟步进（Kernel-General 1）。

## 6. 未完成项与下一步

1. **候选窗口真实可见 + 选择替换**：核心未证项。下一轮只需 **1 次 UAC**：
   agent 已具备节奏化打字与 25 秒候选等待，第 6 轮的时序证据（就绪点 04:17:40 vs
   打字起点 04:17:41）表明 400ms 键距足以让冷启动在打字中途完成。
2. **态 A/B/C 真实矩阵**：候选可见后才能观测（记事本落在哪个态仍是未知事实；
   `candidate_display` 默认值决策依赖它）。
3. DoD 2 的 `tsf_registration=yes` 侧（注册后 `--diagnose`）。
4. HKCU 每用户注册是否被语言栏采纳（R4）。
5. 6 次 UAC 预算已用尽：**继续 C 层需负责人追加授权**。

## 7. 移交清单

| 项 | 值 |
| --- | --- |
| 起点 HEAD | `5c0f11b` |
| 本轮提交 | 无（AGENTS §9；4 个工具修复未提交） |
| UAC 消耗 | 6 次（预算 5–6，全部批准、无一次取消） |
| 注册身份 | CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}`；en-US profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}` |
| 注册/卸载验证 | 6 ×（注册 S_OK + 卸载 + 逐项读回 absent） |
| 真实宿主证据 | 第 2、6 轮 `rime_dll_loaded_before_typing=True`；第 6 轮 9 键真实键入 |
| 清理/残留 | 六轮 `cleanup_verified=True`；无注册残留 |
| 未执行项 | 候选可见性、选择替换、态矩阵、DoD 2 yes 侧、R4 |
| 阻塞 | UAC 预算用尽，需追加授权；工具修复未提交，建议负责人确认后入库 |
