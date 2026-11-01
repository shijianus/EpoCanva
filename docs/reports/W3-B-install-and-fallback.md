# W3-B 报告 — 正式包闭环复验、可见候选回退与安装/卸载路径

签发日期：2026-10-31
起点 HEAD：`7e7d644`（`docs(w3a): record the preflight-first agent and three clean preflights`）
目标内部版本：`0.2.6-w3b`
交付定位：**内部 Alpha，未分发**（EpoCanva 许可证未定；词表跨法域再分发未决）
执行者：自动化协作者（本机、非提权会话）

> **任务书说明**：用户指定的 `docs/product/release-gate.md` 在仓库中不存在
> （`git ls-files` 无此路径，工作区亦无）。当前唯一任务书是
> `docs/task-briefs/W3-B-install-and-fallback.md`（标题即“W3-B 任务书”，签发日期
> 2026-10-31），本报告按该文件执行。若负责人本意是另一份文件，请指出，本报告的
> 代码改动与验证仍可复用，但范围判定需重做。

---

## 1. 环境摘要（已脱敏）

| 项 | 值 |
| --- | --- |
| OS | Windows 11 10.0.26200（x64） |
| 编译器 | MSVC 19.44.35209 / 工具集 14.44.35207 |
| CMake / CTest | 3.31.6-msvc6（VS 2022 Professional 自带，不在常规 PATH） |
| Windows SDK | 10.0.26100.0 |
| 构建目录 | `<repo>/build/windows-product`（Release）；`<repo>/build/windows-tsf/test`（独立 TSF 骨架） |
| 仓库根 | `<repo>`（OneDrive 同步目录；I/O 抖动为已知风险） |
| 会话权限 | 普通用户，**未提权**；**未申请 UAC**；**未执行真实注册表写入** |
| 网络 | 无运行期网络 I/O；本轮未下载/安装任何工具链 |

工作区未提交（AGENTS §9：未获明确要求不提交）。`git status` 显示 35 个已修改文件 +
6 个新增源/资源文件；`docs/task-briefs/` 本身是未跟踪的输入文件。

---

## 2. 完成定义（DoD）逐条判定

| # | DoD | 判定 | 依据 |
| --- | --- | --- | --- |
| 1 | 正式包注册→语言栏→真实宿主打字→候选可见→选择→停用→卸载→残留为零 | **未执行** | 需要一次人工 UAC 授权；本轮未申请。W3-A 的 trace 自绘结果未被当作证据 |
| 2 | `--diagnose` 的 `tsf_registration` 真实反映注册态（yes/no） | **部分验证** | 未注册态实测 `no`；`package=ready`、`manifest=ok`；`yes` 侧与卸载后 `no` 侧未执行 |
| 3 | 产品 DLL 恰好四个导出 | **通过（B 层）** | `dumpbin /exports`：4 functions / 4 names，见证据 `dumpbin-exports-product.txt` |
| 4 | 全量回归 152 条零失败；跳过项显式 `SKIP_RETURN_CODE 77` | **通过** | `100% tests passed, 0 tests failed out of 152`；1 条跳过：`windows.tsf_context_observer.real_focus`（退出 77，非静默） |
| 5 | 每条新增能力有能失败的用例 + `*can_fail` 元测试，失败态真实触发并留日志 | **通过（B 层）** | `product.ui.candidate_display_can_fail`、`product.unit.registration_can_fail` 均实测退出 14 且无 `RESULT PASS`；日志在证据目录 |
| 6 | `EpoCanva.rc` 版本四字段与 `VERSION` 一致；产品 DLL 带图标 | **通过** | `product.unit.settings_manifest_paths` 读取 `VS_FIXEDFILEINFO` 与 `ExtractIconEx` 校验 EXE/DLL |
| 7 | 卸载后 HKLM/HKCU 键、两个类别、profile absent；语言栏无 EpoCanva；目录可删 | **未执行** | 无 UAC 授权，未写真实注册表；真实卸载验证未做 |
| 8 | 文档与二进制行为同步 | **通过** | README、requirements §7、tsf-bootstrap、isolation-verification-plan、tsf-engine-lifetime-contract、candidate-ui-contract、overview 已改 |

**结论：W3-B 为“部分验证”。** B 层（受控、无注册表写入）全部通过；C 层（真实 UAC
注册/激活/卸载、正式包闭环）未执行，因此**不能写“W3-B 已交付”**。

---

## 3. 工作项结果

### W3-B.1 正式 r3 包闭环复验 — 未执行

- 未申请 UAC，未运行 preflight 3/3，未注册/激活正式 TIP，未启动真实宿主输入。
- `build/product/stage/EpoCanva-0.2.5-w3a-r3/` 未被当作本轮证据；W3-A trace 轮次的
  stage8 自绘结果也没有被复用（它违反 `pbShow` 契约）。
- 该工作项仍是 C 层闭环的第一项；所需授权内容见 §10。

### W3-B.2 候选可见性与回退显示 — 已实现并完成 B 层验证

**先决事实核查（代码与文档）**

- `ITfUIElementSink::BeginUIElement` 的 `pbShow` 语义按本机 SDK
  `msctf.idl:2686-2691` / `msctf.h:11865-11868` 与微软官方文档核对：
  `TRUE` = 应用不绘制、由文本服务绘制；`FALSE` = 应用接管绘制、文本服务不得再画。
- 任务书表格把“态 A（pbShow=TRUE）”写成“不自绘”，与上述语义相反。本轮按**实际契约**
  实现：态 A 由 TIP 在真实 token 矩形处显示标准候选窗口（这就是 W3-A 冻结的行为）；
  态 B 默认不自绘；态 C 自绘。该措辞冲突在此显式记录，未按错误语义改代码。

**实现**

- 设置项 `candidate_display=host|always_self`：
  - 默认 `host`；缺失该键的旧 `settings.ini` 仍按 `host` 加载（不判 corrupt）；
  - 非法值判 corrupt；
  - 设置 EXE 增加两个单选按钮与一条警告文案（“宿主也可能绘制，会出现两个窗口；
    宿主的显式隐藏仍然优先；下一次引擎启动生效”）；
  - `--diagnose` 输出 `candidate_display=host|always_self`。
- 三态行为（`platforms/windows/tsf_product/candidate_ui.*`）：
  - 态 A（管理器可用，`pbShow=TRUE`）：TIP 窗口，位于真实 token 矩形；
  - 态 B（管理器可用，`pbShow=FALSE`）：默认隐藏；`always_self` 时显示；
  - 态 C（`ITfUIElementMgr` QI 失败）：TIP 自绘；
  - 显式 `Show(FALSE)` **优先于** `always_self`，隐藏后数据更新不重开；`Show(TRUE)` 才
    交回绘制权并清除宿主的 FALSE 决定；
  - `EPOCANVA_TSF_DIRECT_SINK_TEST` 受控变体不进入态 C 自绘，也不创建窗口。
- 锚点仍只来自 `ReadCaretPosition`/`ReadTokenRect`（真实 `GetSelection` +
  `ITfRangeACP::SetExtent` + `ITfContextView::GetTextExt`）；无布局时隐藏/关闭，不伪造；
  40 次/token 重试预算保持不变；无进程名/文件名分支。
- **读取时机**：`settings.ini` 不在激活 STA 上同步读取。`EngineStartTransaction::Prepare`
  （已在 bootstrap 线程读取同一文件做部署摘要）解析并把偏好发布为进程级原子；
  STA 只在菜单更新时读取原子。设计理由：同步读取会改变候选/建议时序（见 §6 回归）。

**B 层测试**

`product.ui.candidate_window` 与 `product.ui.candidate_display_can_fail` 覆盖：
态 A 不触发回退、态 B 默认不自绘、态 B+`always_self` 自绘、`Show(FALSE)` 后不重开、
`Show(TRUE)` 恢复、态 C 自绘且点击携带真实快照 identity/版本/context/generation、
态 A/C 无布局不打开窗口；`settings.ini` 往返、旧文件兼容、非法值拒绝、设置窗口
默认值与保存/重启回读。控制条故意断言“态 B 默认自绘”，必须退出 14。

### W3-B.3 `DllRegisterServer` / `DllUnregisterServer` — 已实现并完成 B 层验证

**导出**：产品 DLL 恰好四个名字（`DllGetClassObject`、`DllCanUnloadNow`、
`DllRegisterServer`、`DllUnregisterServer`），`.def` 均 `PRIVATE`，无
`__declspec(dllexport)`；`dumpbin` 证据见 §8。托管测试 DLL 也带四个名字，但测试只断言
存在、从不调用，故不写注册表。

**写入范围**（只在 `Software\Classes\CLSID\{B440F702-…}` 下）：

- `InprocServer32` 默认值 = DLL 绝对路径；`ThreadingModel=Apartment`；
- 标记值 `EpoCanvaProduct=epocanva_tsf`；
- `Implemented Categories\{34745C63-…}`（keyboard）与
  `{49D2F9CF-…}`（UIElement-enabled）。

不注册 profile、不调用 `ActivateProfile`、不改语言栏/输入法选择；profile 仍由
`epocanva_tip_profile_helper` 显式操作。

**安全语义**

- 逐项读回；任一步失败回滚本轮创建/修改的全部键值；
- 外来 CLSID（无本产品标记且 `InprocServer32` 不指向 `epocanva_tsf.dll`）在第一次写之前拒绝；
- 已存在非 `Apartment` 的 `ThreadingModel` 拒绝修改；
- `DllRegisterServer` 作用域选择：已有本产品注册的 hive 原位更新；否则提权进程写 HKLM、
  普通进程写 HKCU；
- 注册窗口计入 `DllCanUnloadNow`：序列期间 `S_FALSE`（新增
  `g_registration_active`）；
- 两个导出都有 `try/catch`，异常不穿越 COM 边界。

**未核实**：HKCU 每用户注册是否被 TSF 语言栏真正采纳（R4）仍需真实 C 层轮次；本轮
只证明策略与键集合，不承诺“每用户注册可免 UAC 且系统可见”。

**B 层测试**（`product.unit.registration_fake_registry`，内存后端，绝不碰真实注册表）：
新建/幂等、标记/路径/线程模型/两个类别、读回、注入写失败后回滚为零、写入期间
`DllCanUnloadNow=S_FALSE`、外来 CLSID 拒绝且原键不变、非 Apartment 拒绝、
反注册后内容 absent、反注册保留外来键、HKLM/HKCU 两个作用域。控制条故意断言
“失败写入仍留下键”，必须退出 14。

**未执行**：真实 HKLM/HKCU 写入；连续两次真实 `DllRegisterServer` 的幂等验证；
真实卸载后两 hive + profile + 语言栏 absent 的验证。B 层用同一实现 + 假后端覆盖了
幂等/回滚/absent 的逻辑，但不冒充真实注册表结果。

### W3-B.4 安装器 / 卸载器 — 未执行（工具链缺失）

只读探测结果（`installer-toolchain-probe.txt`）：

| 工具 | 结果 |
| --- | --- |
| `candle.exe` / `light.exe` / `wix.exe`（WiX） | 不在 PATH；`v3.11`/`v3.14` 常见目录均不存在 |
| `ISCC.exe`（Inno Setup 6） | 不在 PATH；常见目录不存在 |
| `makensis.exe`（NSIS） | 不在 PATH；常见目录不存在 |
| `msixmgr.exe` | 不在 PATH |
| `MakeAppx.exe` | 存在于 `Windows Kits\10\bin\10.0.26100.0\x64`，但不在 PATH |

`MakeAppx` 不能替代本任务：MSIX 需要包身份与签名才能安装，本轮禁止签名、禁止产出
分发物；把未签名 MSIX 当作安装器验收会违反 §2 门禁。因此按任务书要求**停止该工作项**，
不下载/安装任何工具链，标记“未执行（工具缺失）”。安装后的 `%ProgramFiles%` 布局、
卸载顺序与机器可读 JSON 均未实现。

### W3-B.5 C 层端到端验收 — 未执行

未申请 UAC，未注册 TIP，未启动真实宿主，未做态 A/B/C 的真实观测、点击焦点、控制键
降级、锚点或停用/卸载矩阵。W3-A 的 trace 证据未被冒充为本轮正式包结果。

### W3-B.6 版本、图标与文档 — 已完成

- `platforms/windows/product/VERSION` → `0.2.6-w3b`；
- `EpoCanva.rc`：`FILEVERSION/PRODUCTVERSION 0,2,6,0`，
  `FileVersion/ProductVersion "0.2.6.0"`，`1 ICON "EpoCanva.ico"`；
- 新增 `EpoCanvaTsf.rc`：产品 DLL 的图标与 VERSIONINFO（`FileDescription` 标注内部 Alpha）；
- 图标由 `tools/product/make_app_icon.py` 离线生成（原创几何图形，无第三方素材，
  16/32/48 三种尺寸）；设置窗口类使用该图标；
- `default.yaml` 的 `config_version` 与 schema 的 `version` 同步为 `0.2.6-w3b`，并注明
  它们是产品内部包版本，不是 Rime ABI；
- `product_paths.cc` 的编译期兜底版本改为 `0.2.6-w3b`；
- 文档同步：README 阶段表新增 W3-A/W3-B 行并修正“无生产词库”的事实漂移；
  requirements §7 重写；tsf-bootstrap、isolation-verification-plan、
  tsf-engine-lifetime-contract、candidate-ui-contract、architecture/overview 的
  “无注册/导出缺失”表述全部更新；
- `product.unit.settings_manifest_paths` 新增版本/图标校验（EXE 与 DLL 的
  `FILEVERSION`、`PRODUCTVERSION` 与 VERSION 一致；两者至少一个图标资源）。

### W3-B.7 报告 — 本文件

---

## 4. 文件变更

**新增**

- `platforms/windows/tsf/registration.{h,cc}`
- `platforms/windows/product/EpoCanvaTsf.rc`
- `platforms/windows/product/EpoCanva.ico`（生成物）
- `platforms/windows/product/product_display_preference.h`
- `platforms/windows/product/tests/registration_tests.cc`
- `tools/product/make_app_icon.py`

**修改（源码/资源）**

- `platforms/windows/tsf/{CMakeLists.txt,dll_main.cc,epocanva_tsf.def,module_state.{h,cc}}`
- `platforms/windows/tsf_product/{candidate_ui.{h,cc},text_service_controller.cc}`
- `platforms/windows/product/{CMakeLists.txt,EpoCanva.rc,VERSION,product_diagnostics.cc,
  product_paths.cc,product_settings.{h,cc},settings_window.{h,cc},ui_ids.h,
  product_engine_config.cc}`
- `platforms/windows/product/tests/{candidate_ui_tests.cc,product_unit_tests.cc,ui_tests.cc}`
- `tests/windows/tsf_lifecycle/{main.cc,hosted/CMakeLists.txt,
  hosted/epocanva_tsf_hosted_test.def,hosted/hosted_main.cc}`
- `tools/product/isolation/register_tip.ps1`
- `data/product/{default.yaml,epocanva_english.schema.yaml}`

**修改（文档）**

- `README.md`
- `docs/product/requirements.md`
- `docs/architecture/overview.md`
- `docs/windows/{candidate-ui-contract.md,isolation-verification-plan.md,
  tsf-bootstrap.md,tsf-engine-lifetime-contract.md}`

**未提交**：按 AGENTS §9，本轮没有 `git commit`/`push`/tag/PR。

---

## 5. 实际执行的命令与退出码

| 命令（要点） | 退出码 | 证据 |
| --- | --- | --- |
| `cmake --build build/windows-product --config Release`（全量） | 0 | `evidence/full-build.log` |
| `ctest --test-dir build/windows-product -C Release --no-tests=error --output-on-failure` | **0**；152 条，0 失败，1 跳过 | `evidence/full-ctest-final.log` |
| `cmake -S platforms/windows/tsf -B build/windows-tsf/test …` | 0 | `evidence/tsf-standalone-configure.log` |
| `cmake --build build/windows-tsf/test --config Release` | 0 | `evidence/tsf-standalone-build.log` |
| `ctest … -R "^tsf\.lifecycle\."`（独立骨架） | 0；3/3 | `evidence/tsf-standalone-ctest.log` |
| `dumpbin /exports build/windows-product/product/bin/epocanva_tsf.dll` | 0；4 names | `evidence/dumpbin-exports-product.txt` |
| `stage_product.ps1 -Replace` | 0；21 载荷文件，4,368,056 字节 | `evidence/stage-product.log` |
| `EpoCanva.exe --diagnose <文件>`（staged 包） | 0；`package=ready`、`manifest=ok`、`tsf_registration=no` | `evidence/diagnose-staged.txt` |
| `epocanva_candidate_ui_tests.exe --self-check-fail` | **14**（预期失败） | `evidence/candidate_display_can_fail.log` |
| `epocanva_registration_tests.exe --self-check-fail` | **14**（预期失败） | `evidence/registration_can_fail.log` |
| 安装器工具链只读探测 | 0；WiX/Inno/NSIS 缺失 | `evidence/installer-toolchain-probe.txt` |
| HEAD 基线对照：干净 HEAD 构建 + 两条 sink 用例 | 0；2/2 通过 | `evidence/head-baseline-configure.log` 与构建目录 |

**未执行**：任何 `regsvr32`、`DllRegisterServer`/`DllUnregisterServer` 的真实调用、
任何 HKLM/HKCU 写入、profile 注册/激活、UAC 申请、真实宿主输入。

---

## 6. 测试结果与回归处理

### 6.1 最终结果

- 产品树 CTest：**152 条，151 通过，0 失败，1 跳过**。跳过项
  `windows.tsf_context_observer.real_focus` 用 `SKIP_RETURN_CODE 77` 显式声明，
  不是静默通过。
- 新增用例：`product.ui.candidate_display_can_fail`、
  `product.unit.registration_fake_registry`、`product.unit.registration_can_fail`；
  加上扩展后的 `product.ui.candidate_window`、`product.unit.settings_manifest_paths`、
  `product.ui.settings_window`。
- 独立 TSF 骨架：3/3 通过（导出断言已反转为“四个都存在”）。
- 产品 DLL SHA-256：`5BE01155EFF508ADB3DE4BE09C9447111233DDA841E30BC8BC383F17962AA604`。

### 6.2 本轮引入并修复的回归（确认缺陷）

**症状**：`product.integration.sink_candidate` 与 `product.integration.sink_degraded_keys`
在 `pbShow`/退格/Escape 序列中出现“菜单已开但按键不被吃”，11–14 条断言失败。
干净 HEAD 构建（`build/w3b-head-baseline-build`）同样两条用例 **2/2 通过**，证明是
本轮改动引入的回归，而不是既有缺陷或环境抖动。

**根因**：`ProductTextController::Start` 在激活 STA 上同步读取 `settings.ini`。
该读取本身正确，但它改变了引擎启动/建议队列的时序，使测试在建议队列尚未稳定时
发送方向键/Escape；产品按设计拒绝在建议在途/过期时吃键，于是断言失败。

**修复**：把偏好读取移到 `EngineStartTransaction::Prepare`（bootstrap 线程，已经在读同一
文件做部署摘要），发布为进程级原子；STA 只在菜单更新时读原子。功能不变，STA 不再有
额外文件 I/O。修复后两条用例恢复通过，全量 152 条零失败。

### 6.3 结果分类（按任务书 §W3-B.7）

- **确认缺陷**：上述“同步读取设置扰动输入时序”回归（本轮引入、本轮修复，最终代码不含）。
  未发现其他确认缺陷。
- **写入结果不确定**：无。本轮没有真实注册表写入，因此不存在“写了但结果不确定”。
- **过期拒绝**：无。没有使用旧授权或旧包结果。
- **用户明确取消或未授权**：W3-B.1、W3-B.5、真实注册/卸载验收（DoD 1/2 的 yes 侧/7）
  均因**未申请到 UAC 授权**而未执行；W3-B.4 因工具链缺失未执行。这些不计入通过。

---

## 7. 关键设计选择与被拒方案

1. **用户设置而非宿主白名单**：`always_self` 是显式、可持久化的用户选择；拒绝按进程名
   分支（W3-A 已删除白名单，本轮不复活）。
2. **态 C 自绘**：`ITfUIElementMgr` 不存在时 TSF 没有“宿主绘制决定”可遵从，自绘是规范
   允许的降级；拒绝把它表述为“违约捷径”。
3. **`Show(FALSE)` 优先于 `always_self`**：用户的回退设置不能覆盖宿主当次的显式隐藏；
   拒绝“设置开启后无条件弹窗”。
4. **异步读取偏好**：拒绝在 STA 上同步读 INI（实际造成回归）；接受 bootstrap 线程解析 +
   进程原子发布。
5. **注册由 DLL 自己完成**：拒绝继续依赖 PowerShell 脚本作为唯一注册手段；脚本保留为
   独立隔离工具。`DllRegisterServer` 只写 CLSID/两个类别，拒绝把 profile 注册塞进 COM
   自注册（会改变用户输入法选择）。
6. **假注册表后端**：B 层测试拒绝写真实 HKCU/HKLM；同一策略在内存后端上验证，
   真实写入留给授权 C 层。
7. **安装器不即兴替代**：工具链缺失时拒绝手写“复制文件 + regsvr32”的临时 installer
   冒充 W3-B.4 交付；按任务书停止并报告缺失项。

---

## 8. 证据目录

`<repo>/build/w3b-b-layer/evidence/`

- `full-build.log`（全量构建，exit 0）
- `full-ctest-final.log`（152 条，0 失败，1 跳过，ctest exit 0）
- `dumpbin-exports-product.txt`（4 个导出）
- `candidate_display_can_fail.log`、`registration_can_fail.log`（故意失败，exit 14）
- `tsf-standalone-{configure,build,ctest}.log`（独立骨架 3/3）
- `stage-product.log`、`staged-dll-sha256.txt`、`diagnose-staged.txt`
- `installer-toolchain-probe.txt`
- `head-baseline-configure.log`（HEAD 对照构建；二进制在
  `<repo>/build/w3b-head-baseline-build`）
- 暂存包：`<repo>/build/product/stage/EpoCanva-0.2.6-w3b/`
  （21 载荷文件 + `package.manifest`；载荷 4,368,056 字节，总 4,370,235 字节）

注意：`rime.dll` 仍内嵌开发机绝对路径（stage 脚本报告 `D:\Users\`×67、`OneDrive`×68、
`Smart Kevin`×68）。这是公开分发的已知门禁，本轮未修，原样转写。

---

## 9. 未完成项、风险与阻塞

**未完成**

1. W3-B.1 正式包闭环复验（UAC+真实宿主）。
2. W3-B.5 C 层矩阵（态 A/B/C 真实观测、点击焦点、控制键、锚点、停用/卸载）。
3. DoD 2 的 `tsf_registration=yes` 与卸载后 `no` 两侧。
4. DoD 7 的 HKLM/HKCU/profile/语言栏/目录残留验证。
5. W3-B.4 安装器/卸载器（WiX/Inno/NSIS 缺失；未下载安装）。
6. 每用户（HKCU）注册是否被 TSF 语言栏采纳（R4）。
7. 若主流宿主落在态 B，“用户看得见候选”仍依赖用户手动翻设置（R2 决策点）。

**风险**

- R1 授权脆弱：一次 UAC 取消即整轮作废；preflight 必须 3/3 绿才申请。
- R2 默认值：本轮未擅自改 `always_self` 默认；若 C 层显示主流宿主为态 B，
  是否改默认需负责人决定。
- R3 Windows 11 Notepad 会话/标签复用：测试文件绝对路径、进程数为 0、窗口归属可证明
  才操作（W3-A §11.12 的三条修复仍是前提）。
- R4 每用户注册未核实：不得据印象承诺免 UAC 的系统可见性。
- R5 安装工具链缺失：W3-B.4 未执行；需负责人决定是否允许安装 WiX/Inno（AGENTS §9 要求
  明确授权，本轮未自行安装）。
- R6 已被映射的旧 stage 文件删不掉：卸载验收须区分注册残留与构建产物残留。
- R7 引擎异步启动失败会在本会话保持 literal：本轮未引入自动重启。
- R8 OneDrive 作为构建根：I/O 抖动会影响时序敏感测试；本轮的同步设置读取回归就是
  时序敏感性的实证。

**阻塞下一阶段的事项**

1. 一次（或多次）明确的 UAC 授权，按 W3-A §11.10 / W3-B §6 的 session 表执行；
   未授权前不得注册、不得激活、不得启动真实宿主输入。
2. 安装器工具链的负责人决策：允许安装 WiX/Inno/NSIS，或接受 W3-B.4 保持“未执行”。
3. `docs/product/release-gate.md` 缺失的确认：本报告按 `W3-B-install-and-fallback.md` 执行。

---

## 10. 授权申请（待负责人批准后执行）

沿用 W3-A §11.10 表格，一次一 session。拟申请内容：

| 项 | 值 |
| --- | --- |
| 源码 | 工作区基于 `7e7d644` + 本报告所述未提交改动（无提交 SHA；负责人可要求先提交） |
| 包路径 | `<repo>/build/product/stage/EpoCanva-0.2.6-w3b/` |
| DLL SHA-256 | `5BE01155EFF508ADB3DE4BE09C9447111233DDA841E30BC8BC383F17962AA604` |
| 注册身份 | CLSID `{B440F702-63E4-4D47-BCC4-074899ABA836}`；en-US profile `{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}`；`GUID_TFCAT_TIP_KEYBOARD` + `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` |
| 键范围 | HKLM/HKCU `Software\Classes\CLSID\{B440F702-…}` 下的 `InprocServer32`、两个 `Implemented Categories` 子键、`EpoCanvaProduct` 标记；profile 由 helper 显式注册/激活 |
| 应用 | 全新 Notepad（`RichEditD2DPT`）为主；态 C 需至少 4 个真实宿主（含无 UIElementMgr 的宿主） |
| 预算 | W3-B.1 1 次；W3-B.2 回退后 ≥2 次；W3-B.4 1 次（若工具到位）；W3-B.5 1–2 次；合计 5–6 次 UAC |
| 清理 | deactivate → unregister profile → `DllUnregisterServer`/`unregister_tip.ps1` → 读回 HKLM/HKCU/两类别/profile absent |
| UAC 取消 | 立即停止，不重试、不复用旧批准 |
| 证据 | 每轮 `build/w3b-r3-verify/evidence-<session>/`，含任务书 §7 的最小集 |

---

## 11. 移交清单

| 项 | 值 |
| --- | --- |
| 起点 HEAD | `7e7d644` |
| 本轮提交 | **无**（AGENTS §9；工作区改动未提交） |
| 变更文件 | 见 §4（35 修改 + 6 新增） |
| 冻结包 | `<repo>/build/product/stage/EpoCanva-0.2.6-w3b/`，21 载荷文件，4,368,056 字节 |
| DLL SHA-256 | `5BE01155EFF508ADB3DE4BE09C9447111233DDA841E30BC8BC383F17962AA604` |
| `--diagnose` | `version=0.2.6-w3b`、`commit=7e7d644`、`package=ready`、`manifest=ok`、`manifest_files=21`、`manifest_failures=0`、`settings=missing`、`candidate_display=host`、`tsf_registration=no` |
| CTest | 152 条；151 通过、0 失败、1 跳过（`windows.tsf_context_observer.real_focus`，77） |
| UAC 授权 | 0 次（未申请） |
| 清理/残留 | 本轮无真实注册表写入，因此无注册残留；暂存包与构建产物在 `build/` 下 |
| 未执行项 | W3-B.1、W3-B.4（工具缺失）、W3-B.5、DoD 1/2-yes/7；确切缺失授权或工具见 §9 |
| Open questions | `release-gate.md` 是否存在；态 B 为多数宿主时是否改默认；WiX/Inno 是否允许安装；HKCU 注册的系统可见性；`rime.dll` 内嵌路径修复 |
