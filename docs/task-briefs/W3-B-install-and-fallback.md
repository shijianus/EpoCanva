# W3-B 任务书 — 正式包闭环复验、可见候选回退与安装/卸载路径

签发日期：2026-10-31
起点 HEAD：`7e7d644`（`docs(w3a): record the preflight-first agent and three clean preflights`）
冻结对照包：`build/product/stage/EpoCanva-0.2.5-w3a-r3/`（源码 `488b6f5`，
`epocanva_tsf.dll` SHA-256 `911B63A3FB79BFC26A873E879A4A9B5661A4DE938121D1961AB78C62CC5E8294`）
目标内部版本：`0.2.6-w3b`
交付定位：**内部 Alpha，不得对外分发**（理由见 §2）
上位约束：`AGENTS.md` §1–§10 全部生效；本报告若与任务书冲突，以任务书为准并报告冲突。

---

## 1. 负责人已锁定的三项范围决定

| # | 决定 | 对任务的影响 |
| --- | --- | --- |
| D1 | W3-B 边界 = 正式包闭环复验 + `DllRegisterServer`/`DllUnregisterServer` + 安装器/卸载器 + 图标与版本资源 | 复现性/CI、英文能力、中文、崩溃安全模式、DPI **不在本轮**（见 §8） |
| D2 | 本轮**加入宿主拒绝绘制时的候选回退显示** | 需要新增用户显式设置项与 B/C 层契约测试；**禁止**进程名白名单 |
| D3 | 验证环境仍为**本机 + 精确一次性 UAC 申请**，VM/Windows Sandbox 不启用 | 每轮 C 层验证都消耗一次人工授权；§6 的授权预算必须按最坏情况规划 |

---

## 2. 前置门禁（不满足则对应工作项不启动，不得绕过）

| 门禁 | 状态 | 处理 |
| --- | --- | --- |
| EpoCanva 自身许可证未定（AGENTS §8；`third_party/sources.lock.json` 记录"迄今没有任何产物被批准对外分发"） | 未决 | W3-B 交付安装器**技术实现**，但安装包不签名、不产出分发物；所有产物标注内部 Alpha |
| 词表跨法域再分发结论未决（`docs/compliance/english-pilot-data.md` §5；provenance `open_decision=redistribution-outside-internal-alpha-needs-owner-legal-review`） | 未决 | 同上；安装器验收只在当前开发机内完成 |
| `rime.dll` 内嵌开发机绝对路径（`D:\Users\…`×67、`OneDrive`×68）仍是公开分发门 | 未修 | **不在本轮修复**；但 W3-B 报告必须复核该 WARNING 仍在，并原样转写 |
| 安装器工具链（WiX / Inno Setup / MSIX）本机是否可用 | **未知** | W3-B.4 第一步必须是只读探测并如实报告缺哪个；禁止"顺手下载安装器制作工具"（AGENTS §9） |

---

## 3. 完成定义（DoD）— 逐条可判定

全部满足才可写"W3-B 已交付"。任一条为红或未执行，结论写"部分验证"并列出阻塞。

1. 用**未启用 trace 的正式包**（`EPOCANVA_PRODUCT_TRACE=OFF`）在本机完成一次注册 → 语言栏可选 → 真实宿主打字 → **候选窗口肉眼可见** → 选择 → 仅目标 token 改变 → 停用 → 卸载 → 残留为零 的闭环，且证据目录含 §7 要求的最小证据集。
2. `EpoCanva.exe --diagnose` 输出 `tsf_registration=` 能真实反映注册态（注册后为 `yes`、卸载后为 `no`），且 `package=ready`、`manifest=ok`。
3. 产品 DLL 导出表含且仅含 `DllGetClassObject`/`DllCanUnloadNow`/`DllRegisterServer`/`DllUnregisterServer`；`dumpbin /exports` 输出进证据目录。
4. 全量回归：产品树 `ctest --no-tests=error` 149 条（或新增后的准确条数）零失败；跳过项必须用 `SKIP_RETURN_CODE 77` 显式声明，不得静默通过。
5. 本轮新增的每条能力都有**一个能失败的用例**，且各带一条 `*can_fail` 元测试；新增测试的失败态被实际触发过一次并把日志留在证据目录。
6. `EpoCanva.rc` 的 `FILEVERSION`/`PRODUCTVERSION`/`FileVersion`/`ProductVersion` 与 `platforms/windows/product/VERSION` 一致，产品 DLL 带图标资源。
7. 卸载后验证：HKLM 与 HKCU 本产品 COM/TIP 键、两个实现类别、profile 全部 absent；语言栏不再出现 EpoCanva；`EpoCanva.exe` 目录可被删除（若仍被应用映射，按 §9-R6 记录为已知限制而非通过）。
8. 报告与 `README.md`、`docs/product/requirements.md` §7、`docs/windows/tsf-bootstrap.md`、`docs/windows/isolation-verification-plan.md`、`docs/windows/tsf-engine-lifetime-contract.md` 中所有"无注册/无安装器/导出缺失"表述同步更新；文档不得残留与二进制实际行为矛盾的句子。

---

## 4. 工作项（严格顺序；不得并行跳步）

### W3-B.1 正式 r3 包闭环复验（先测量，后改代码）

目的：拿到**第一个关于严格契约包的确定结论**，再动代码。W3-A §11.9 第 1 步欠的账。

- 前置：按 W3-A §11.12 的 preflight-first 握手，先跑 3/3 无注册、无输入预检，`raw_return=true`/`win32_error=0` 且窗口归属可证明，才允许申请 UAC。
- 输入 `alpha hel`，断言按 `BeginUIElement` 的实际返回值分支记录：
  - 宿主返回 TRUE 且候选窗口可见 → 点击第 1 行（非 RAW 行），验证 `alpha ` 前缀保留、焦点仍在文档；
  - 宿主返回 FALSE 且不自绘 → 判为 **`host_presentation_refused`**，这是**预期的契约行为结果**，不是产品缺陷也不是通过；在候选窗口缺失处停止，不发送任何选择输入。
- 禁止：把 W3-A trace stage8 的自绘结果当成本轮证据；用 Down 次数硬编码代替真实行内容。
- 产出：`build/w3b-r3-verify/evidence-<session>/`；结论原样写入报告，若为 `host_presentation_refused` 则该结论成为 W3-B.2 的**输入事实**。

### W3-B.2 候选可见性与回退显示

先决事实核查（不得凭印象，须实测并引用返回值）：
- 宿主 `ITfSource::QueryInterface(IID_ITfUIElementMgr)` 在 Notepad（`RichEditD2DPT`）及至少 4 个其他真实宿主上的成败；
- 现有代码在 **UIElementMgr 不可用**分支的实际行为（当前实现是否等价于"永不自绘"）。

设计规则（三类宿主态，行为必须不同且互不混淆）：

| 态 | 判据 | 本轮要求的行为 |
| --- | --- | --- |
| A 宿主有 UIElement 且返回 TRUE | `BeginUIElement` → `pbShow=TRUE` | 保持 W3-A 冻结契约：不自绘，窗口位于真实 token 矩形 |
| B 宿主有 UIElement 但返回 FALSE / `Show(FALSE)` | `pbShow=FALSE` | 默认**仍不自绘**；仅当用户显式设置 `candidate_display=always_self` 时自绘 |
| C 宿主根本没有 `ITfUIElementMgr` | QI 失败 | 自绘（TSF 规范允许的降级，不表述为"违约捷径"） |

硬约束：
- **永久禁止按进程名/宿主名分支**。W3-A 已删除 Notepad 白名单，本轮不得以任何形式（含"临时诊断""host 兼容表"）复活。
- 设置项落在设置 EXE，默认 `host`（跟随宿主）；`always_self` 的界面文案必须明说"宿主可能同时绘制候选，会出现两个窗口"的风险；选项必须持久化并在下一次引擎启动生效，生效时机写进文档与测试。
- 任何自绘路径必须满足：`WS_EX_NOACTIVATE`、`WM_MOUSEACTIVATE=MA_NOACTIVATE`、锚点**只**来自真实 `GetTextExt`/`ReadTokenRect` 的 token 矩形、无布局时**关闭菜单而不是伪造矩形**、anchor 重试预算沿用 W3-A 的 40 次/token 且不因候选版本变化重置。
- B 层新增可失败用例：态 B 默认不自绘 / 态 B + `always_self` 自绘 / 态 C 自绘 / 态 A 不自绘 / `Show(FALSE)` 后不重开 / 无布局不伪造矩形。

**决策点（须回报负责人，不得自行扩大范围）**：若 W3-B.1 与 W3-B.2 实测显示主流宿主普遍返回态 B，则"用户看得见候选"依赖用户手动翻设置。此时把实测矩阵提交负责人，由其决定是否把默认值改为 `always_self`；本轮**不擅自改默认值**。

### W3-B.3 `DllRegisterServer` / `DllUnregisterServer`

- 语义：只写本产品自己的键（CLSID/`InprocServer32`/两个实现类别），逐项读回验证，任一步失败**回滚本轮已写的全部键**；遇到已被其他产品占用的 CLSID 直接拒绝并给出可读错误。
- 双作用域：HKLM（每机）与 HKCU（每用户）都必须支持。**"每用户注册可免 UAC"是待核实的假设**：须核对微软文档/头文件与本机真实行为后才能写进文档；未核实前一律按需要管理员处理并如实报告。
- 不动语言用户界面设置：不调用 `ActivateProfile` 之外的任何"切换用户输入法"动作；profile 的注册/激活仍走 `tip_profile_helper` 的显式调用路径。
- **必须同步反转的既有断言**（漏掉会直接编译/测试转红）：
  - `tests/windows/tsf_lifecycle/hosted/hosted_main.cc:312-315` 现断言两个导出 `== nullptr`；
  - `platforms/windows/tsf/epocanva_tsf.def:15-18` 的"deliberately ABSENT"注释与 `EXPORTS` 段；
  - `tests/windows/tsf_lifecycle/hosted/epocanva_tsf_hosted_test.def:6`；
  - `docs/windows/tsf-bootstrap.md:26,180,246`、`docs/windows/tsf-engine-lifetime-contract.md:61`、`docs/windows/isolation-verification-plan.md:154`。
  新的生命周期测试必须**同时**证明：导出存在、注册幂等（连续两次 `DllRegisterServer` 结果相同）、`DllCanUnloadNow` 在注册表写入期间为 `S_FALSE`、卸载后读回 absent。
- `EPOCANVA_TSF_DIRECT_SINK_TEST` 变体继续不进包。

### W3-B.4 安装器 / 卸载器

第一步是只读探测，不是写代码：确认本机可用的安装工具链，缺失则按 AGENTS §3 报告确切缺失项并**停止该工作项**，标记"未执行（工具缺失）"。

- 每机安装：文件落 `%ProgramFiles%\EpoCanva`，用户数据仍在 `%LOCALAPPDATA%\EpoCanva`（沿用 W1 布局）；`InprocServer32` 指向安装后的绝对路径；语言栏图标来自产品 DLL 自带资源。
- 卸载：先 deactivate/unregister profile，再 `DllUnregisterServer`，再删文件；卸载器必须在宿主应用未占用时才能删净，占用时给出可读结果而不是静默失败。
- 安装/卸载都必须留下机器可读的结果 JSON（写了哪些键、读回值、回滚情况）。
- 打包白名单沿用 `tools/product/stage_product.ps1` 的 required/forbidden 纪律：不进 PDB、测试 DLL、注册脚本、trace 开关、`build/` 任意内容。
- **不做**：代码签名、更新通道、发布物产出（§2 门禁）。

### W3-B.5 C 层端到端验收

按 `docs/windows/isolation-verification-plan.md` §4 矩阵中与本轮相关的行执行，且每项保持 W3-A 的会话握手（单 `session_id`、绝对截止时间、每键前校验 PID/HWND/焦点/en-US 布局/修饰键/令牌完整性、仅操作本轮窗口、finally 清理并验证残留）。

本轮最小集：正式注册+激活+移除+再启用、候选窗口显示（态 A/B/C 各至少一次真实观测）、点击不抢焦点、Enter/Escape/BackSpace 正常与降级、移动/点击后旧候选拒绝、真实 token 锚点、停用/卸载。

### W3-B.6 文档与版本同步

- 版本号 `0.2.6-w3b` 同步到 `platforms/windows/product/VERSION`、`EpoCanva.rc`（当前为 `0,1,0,0` 四处）、`default.yaml`/`epocanva_english.schema.yaml` 的 `config_version`/`version` 口径说明。
- 修正在先的事实漂移：`README.md:45` 仍写"没有：生产词库"，而 W1 起包内已含 42,356 行 `epocanva_en_alpha.tsv`；`docs/product/requirements.md` §7 仍是 P0.1 口径。
- `docs/product/requirements.md` §7 与 README 阶段表按本轮实际结果改写；`README.md` 阶段表新增 W3-B 行，状态用本章同款措辞（"已交付（内部 Alpha；未分发）"），禁止写"已上线"。

### W3-B.7 报告

`docs/reports/W3-B-install-and-fallback.md`，按 AGENTS §11 的七段格式，并显式区分四类结果：**确认缺陷 / 写入结果不确定 / 过期拒绝 / 用户明确取消或未授权**。

---

## 5. 本轮永久禁止项

1. `git commit`/`push`/tag/PR — 除负责人明确要求。
2. 新增任何许可证声明、LICENSE 文件、版权头承诺（AGENTS §8）。
3. 运行期网络 I/O、遥测、崩溃上报、静默更新；诊断日志记录按键/preedit/commit/剪贴板/字段名/应用标识。
4. 按进程名或宿主名分支决定绘制行为。
5. 用 B 层通过冒充 C 层通过；用 trace 构建结果冒充正式包结果；用"全部按键直通、原文没丢"单独充当输入法功能通过。
6. 把 `build/regress-all.ps1`（永不返回非零）当作回归证据。
7. 修改 `third_party/` 内任何文件；补丁一律进 `third_party/patches/` 并在 `docs/build/bootstrap.md` 说明理由。
8. 为通过测试而放宽写入器校验（同文本他处、移动、非折叠选区、外部编辑必须仍拒绝）。
9. 安装/下载任何工具链作为缺失工具的替代 —— 报告确切缺失项。

---

## 6. 授权申请（执行前提交，一次一 session）

沿用 W3-A §11.10 表格式，必填：源码 SHA、stage 绝对路径、DLL SHA-256、注册身份与键范围（CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}`、en-US profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`、`GUID_TFCAT_TIP_KEYBOARD` + `GUID_TFCAT_TIPCAP_UIELEMENTENABLED`）、涉及应用清单、预计用时与硬截止、前台空闲要求、停止/清理条件、UAC 取消处理（立即停止、不重试、不复用）。

预算（按最坏情况）：W3-B.1 复验 1 次；W3-B.2 回退实现后 ≥2 次（态 B 默认 / 态 B `always_self`）；W3-B.4 安装器验收 1 次；W3-B.5 端到端 1–2 次。**合计 5–6 次人工 UAC。** 任何一次取消使该轮判为"未执行"，不计入通过，不复用旧批准。

---

## 7. 每轮证据最小集

1. 输入前文本（固定测试串，如 `alpha hel`）；2. 候选菜单真实截图 + 引擎页数 + `status`；3. 显式选择方式（点击/键盘）与实际选中行文本；4. 选择后文本；5. 焦点仍在原编辑控件（`GetGUIThreadInfo` 或截图）；6. `BeginUIElement` 返回值与所属宿主态（A/B/C）标签；7. `dumpbin /exports`；8. 注册/卸载前后注册表快照与残留验证 JSON；9. 全部命令的真实退出码。

---

## 8. 本轮明确不做（进 open questions，不进代码）

复现性/单一构建入口/CI；修 `rime.dll` 内嵌路径；英文能力（学习、形态、连写分段、缩写展开、把 `enable_suggestions` 默认打开）；中文与其他语言；崩溃安全模式；DPI/多显示器；代码签名与发布；无障碍与设置 UI 本地化；grapheme 级光标移动。

---

## 9. 已知风险与限制

- **R1 授权脆弱**：本机 + UAC 意味着单轮编排失败即整轮作废（W3-A §11.11 已发生一次：`notepad_launcher_pid=0`、9 秒后放弃、无按键）。preflight 未 3/3 绿不得申请。
- **R2 默认值可能仍看不见候选**：若主流宿主落在态 B，DoD 第 1 条依赖用户显式翻设置；须按 W3-B.2 决策点上报，不得自改默认。
- **R3 Windows 11 Notepad 复用会话/标签**：测试文件一律绝对路径、启动前 Notepad 进程数必须为 0、窗口归属不可证明时不关闭任何窗口（W3-A §11.12 的三条修复是本轮前提）。
- **R4 每用户注册免 UAC 未核实**：见 W3-B.3；不得据印象承诺。
- **R5 安装工具链可能缺失**：探测先行，缺失即该工作项"未执行"。
- **R6 已被映射的旧 stage 文件删不掉**：W3-A §8.7（Explorer/Edge/Chrome/Qoder 早前加载过 TIP，Windows 拒绝删除）。卸载验收须区分"注册残留"与"构建产物残留"，后者记录为限制。
- **R7 引擎异步启动失败会在本会话保持 literal**：本轮不引入自动重启来掩盖配置/部署错误。
- **R8 OneDrive 同步目录做构建根**：I/O 抖动会影响构建与测试稳定性，属已知环境风险。

---

## 10. 移交清单模板（报告末尾必填）

起点 HEAD / 每个变更提交 SHA / 变更文件全列表 / 冻结包绝对路径 + 版本 + 载荷文件数与字节数 + DLL SHA-256 / `--diagnose` 关键字段 / ctest 准确条数与跳过项名称 / 每次 UAC 授权的 session 与结果 / 清理与残留验证结论 / 未执行项及确切缺失工具或缺失授权 / 转交 §8 的 open questions。
