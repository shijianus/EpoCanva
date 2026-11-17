# 2026-11-17 内部验收工程审计

状态：**BLOCKED_NOT_COMPLETE / 技术 NO-GO / 公开发行 NO-GO**。
本轮完成可在当前桌面独立验证的修复，不授予数据、宿主、签名或发行批准。
最终执行数量、散列与门禁结果在本报告的执行结果部分记录。

## 环境与审计身份

- 分支：`feat/internal-acceptance-round`；远端与本地 HEAD：
  `80cb0bbdd78825036e062416137fb61a06998734`。
- 已执行该分支的 fetch 和 ls-remote，未改用 main。最近六个提交为
  `80cb0bb`、`0b449da`、`1e073e4`、`48ed516`、`8e7b8b4`、`39176fc`。
- 当前为普通 Windows 11 x64 本机、OneDrive 工作区，未获正式隔离宿主授权。
  PowerShell 5.1.26100.9818、Python 3.14.3、Git 2.52.0、VS CMake/CTest
  3.31.6-msvc6。实际探测原文见 `environment.log`；工具不在 PATH 的事实没有
  通过安装包、改配置或提权绕过。
- 初始已有 `.github/workflows/windows-ci.yml` 修改和未跟踪路径回归测试；原始
  patch、测试文件与 status 已保存在证据目录，没有覆盖其他人的改动。
- 证据根：`build/acceptance-audit-20261116/`（沿用任务开始日期）。本报告中的
  日志文件名均相对该目录。构建与测试产物全部在 `build/<preset>/`。
- 没有执行 git commit/push/tag/PR；AGENTS §9 明确要求另有明确指令。
  因此“新提交列表”为无；工作区修复没有被称为干净候选 SHA。

## 历史 CI 的真实处置（B01）

原始 run：[windows-ci #14 / 43570509740](https://github.com/shijianus/EpoCanva/actions/runs/43570509740)，
root job `132374343144`，来源 SHA 为上述 `80cb0bb`。`github-run-14.json`、
`github-jobs-14.json`、`github-root-14.log` 和解压的 `github-root-artifact-14/`
显示 configure/build/ctest 都返回 0，103 项 CTest 全部通过。
job 在 `verify_root_junit.py` 返回 1：预期位置没有 `ctest.xml`。

实际原因是 workflow 将相对 `--output-junit build/vs2022-x64-debug/ci/ctest.xml`
与 `--test-dir build/vs2022-x64-debug` 同用；CTest 以测试目录解析输出路径，
形成嵌套路径，而 verifier 以调用目录读取。修复保留完整清单/零跳过要求，使用
绝对证据目录、清除旧 JUnit、验证新文件存在且非空。真实 CTest 集成回归证明旧写法
CTest=0/verifier=1，新写法 verifier=0，并覆盖带空格路径及变异负例。

artifact `14103509834` 的本机 SHA-256 为
`a900b765e9d09a9d23ffbb7881121bb812a050c6549234d9141ef627e935ce91`，
与 GitHub artifact digest 相同。首次匿名日志/artifact 下载分别返回 403/401，
失败响应也保留；后续使用既有 credential helper 在内存认证获取原始文件，凭据
没有写入报告或日志。历史 #13 的成功不作为本轮通过证据。

验收条件：新干净候选 SHA 的真实远端 root job 成功，注册/执行清单一致且零失败、
零错误、零跳过；本机回归不能关闭远端候选门禁。

## 逐项问题记录

### B02：焦点通知、失败分类与辅助窗口资源

- 原因：`RunRealFocus` 记录两条失败后仍返回环境码 77，CTest 将其记为 skip。
  本机 `SetFocus`/`GetFocus` 和 Win32 窗口焦点成功，但 TSF `OnSetFocus` 计数仍为 0，
  清除窗口关联后 B target 仍发布。通知缺失的生产/环境根因尚未被独立证明，不能
  用“环境限制”解释为通过。
- 源码：`tests/windows/tsf_context_observer/{observer_tests.cc,observer_test_support.cc,.h}`；
  生产观察器 `platforms/windows/tsf_context/context_observer.cc`。
- 复现：产品 Release 构建后 `ctest --test-dir build/windows-product -C Release
  -R '^windows[.]tsf_context_observer[.](real_focus|environment_failure|resource_balance)$'
  （完整实际参数见命令记录）。
- 影响：焦点切换、异步目标身份、真实宿主正确性和测试诚实性。
- 修复：已有失败时环境分类返回真实失败 5；新增负例要求 exit 5，禁止 PASS 标记；
  驱动并核验实际 Win32 focus。辅助窗口原先仅返回永远为空的静态 HWND，反复
  Ensure 会新建窗口，改为每线程复用并由该线程释放。
- 正向/负向：复用检查 128 次；已有 COM/store/sink 清理断言保留；环境失败负例、
  `can_fail` 与真实焦点原始输出均保留。真实焦点没有通过时不关闭阻断。
- 验收条件：真实通知、目标移除及全部资源检查均通过，并在获授权真实宿主复验。

### B03：完整文档 oracle 与正式压力 entry 的边界

- 原因：旧 `tests/reference_model` 只覆盖 composer/EditPlan，没有完整文档、选区、
  undo 和失败序列最小化；不能被命名成完整编辑器验收。
- 修复/源码：新增 `tests/editor_reference_model`，根工程与产品 CMake 均接入。
  oracle 独立存 Unicode scalar、独立 UTF-8/UTF-16 编码，对照生产编辑 executor
  和真实 msctf.dll 的受控 text store，每步比较完整文档、UTF-16 选区和组合状态。
- 复现/正向：`editor.reference_model.stress`，五固定种子各 100000 操作；插入、
  删除、UTF-16 光标、选区、受控应用 undo、invalid-state 六类均执行。
- 负向：坏 UTF-8、代理对内部光标、Unknown、异步过期/撤权、遗漏写入 mutant；
  检测后 delta-debug 加逐项删除到单操作，落盘并重放相同失败，去掉 mutant 后成功；
  非法/空 replay 和不可写报告拒绝；强制失败返回 1。
- 影响：编辑正确性、Unicode、异步安全、压力证据可信度。
- 剩余：非折叠选区编辑按现有拒绝契约检查；undo 属受控应用自己的 edit session。
  原始测量尚未产出当前安装包/JUnit 绑定的正式 `stress-reference-model` entry，
  不证明安装宿主或资源增长达到尚未批准的绝对性能阈值。
- 验收条件：正式 entry 完整绑定候选、包、runner、JUnit、重放和资源采样，再通过
  aggregate gate；已安装应用行为仍须 B07。

### B04：产品持久学习写入

- 原因/源码：`platforms/windows/product/product_settings.cc` 的
  `ProductLearningWritesPermitted()` 恒 false；`product_engine_config.cc` 按该结果
  配置后端。当前没有覆盖排队→worker→落盘全过程的不可竞态授权与撤销证明。
- 影响：隐私、显式学习、上下文个性化；不能声称产品已完成持久学习。
- 复现：核对该函数及产品/backend 学习测试，测试 Unknown/敏感字段和撤权路径。
- 方案：保留写关闭；后续引入能在执行和写盘前重新验证、撤权可取消队列的授权
  生命周期，补竞态和清除集成验证后才考虑开启。
- 正向：允许的显式选择/拒绝/手动词条语义与本地存储组件测试；负向：Unknown、
  password/OTP、普通 commit/backspace 不产生学习、排队后撤权不落盘。
- 验收条件：全部生命周期和真实敏感字段证明齐全，未分类一律拒绝；本轮没有开启。

### B05：固定候选与提交

- 原因：工作区含修复和初始已有改动，尚无新提交。任务不具有 AGENTS §9 要求的
  明确 commit/push 授权，不能擅自提交来制造 clean SHA。
- 复现：`git status --short`、`git rev-parse HEAD` 和 source-before/after JSON。
- 影响：候选冻结、远端 CI、安装包/Corresponding Source/质量证据统一身份。
- 方案：交付完整差异与逐文件散列供审阅；正式流程将审定内容提交后固定 clean SHA，
  从该 SHA 重建全部证据。dirty fingerprint 是本机内容身份，不是 Git SHA。
- 正向：测试前后 source fingerprint 一致；负向：改源码、二进制或 JUnit 后旧 receipt
  必须拒绝。验收条件：工作区干净、固定 SHA、同一 SHA 完整 CI 和产品测试。

### B06：Tier B、绝对阈值与质量门禁

- 原因：消费者原先相信 report 的 status/hash，没有重新检查当前 lock 的 approved
  内容，也没有将实测 Top-1/Top-3/MRR 重新与批准阈值比较；直接 double 转换可
  接受不合格类型/非有限数。真实仓库的批准资料仍不存在。
- 源码/修复：`tools/product/check_release.ps1` 复核当前锁、Tier B manifest、每语言
  来源/许可/批准引用/重现路径/原文隐私声明/文件散列和实际行数；重算实测阈值，
  严格校验率和性能字段；`tools/backend/candidate_quality_policy.py` 校验 schema
  版本真整数和非法语言容器类型。
- 复现/正向：`candidate_quality_gate_tests.ps1` 的合成 parser fixture；
  `test_candidate_policy.py`。负向：未批准锁但报告通过、Tier A、缺锁、篡改散列/来源、
  below-threshold 仍报 passed、NaN/Infinity/bool/string、错误 schema；对旧 checker
  运行同一回归真实 exit 14、23 个失败断言，说明负例能发现旧缺陷。
- 影响：原文保护、建议质量、错误语言、确定性和性能证据。
- 真实状态：Tier B `not_approved`，absolute quality `pending_product_owner`。
  Tier A 仅 en=2/zh=4/fr=3/ja=4/ko=2；正式 minimum 是 1001/1001/1001/1001/500。
- 外部记录：负责人批准固定数据身份、来源/许可评估范围、no-user-content、重现方式
  后写入 `data/eval/candidate-v1/tier-b.lock.json`，manifest/corpus 散列必须一致；
  事先批准 Top-1/Top-3/MRR 和绝对延迟/内存等性能标准及适用机器/协议，记录于
  `quality-policy-v2.json` 和关联批准文件。现有相对回归界限不等于绝对性能批准。
- 验收条件：真实批准数据/阈值、固定干净候选、独立执行 qualification 和完整质量
  证据。合成 fixture 或自填阈值不能关闭本阻断。

### B07：安装、真实宿主、隐私和浸泡

- 原因：用户已确认没有可用且获授权的隔离 Windows 桌面/runner。
- 源码：`tools/installer/`、`tools/product/run_english_acceptance.ps1`、
  `run_offline_privacy.ps1` 和 `check_release.ps1` 的 host/soak consumer。
- 状态/复现：本轮没有运行系统安装、注册、升级、修复、卸载、回滚、多用户；没有
  已安装 Notepad/Chromium/Word 的五语言输入、英文一致性、敏感字段和 1800 秒浸泡。
  此清单均 NOT_RUN，不能给出不存在的退出码。
- 影响：真实输入、安装安全、学习隐私、离线、焦点、稳定性与资源增长。
- 方案/外部记录：批准环境 ID、runner ID/标签与操作者、隔离及恢复策略、允许的安装
  和注册动作、宿主版本/账户、候选和安装包散列、测试窗口及性能方案；以 hash-bound
  `runner_authorization`/`isolation_authorization` 关联正式 evidence。
- 正向：完整生命周期和 1800 秒持续交互；负向：错误版本、撤权/敏感/Unknown、崩溃
  恢复、安装失败与回滚、跨用户隔离；验收条件：清单完整且零跳过，原始日志完整。
- Windows 普通 TSF DLL 的无网络保证是架构/依赖审计保证；当前本机组件检查不能
  声称 OS 隔离或已安装任意宿主的绝对无网络。Android 需省略 INTERNET 权限，
  当前尚无 Android IME，不存在已执行的平台权限证明。

### B08：六层 CI 与 runner 信任

- 原因/源码：`.github/workflows/windows-ci.yml` 只启用 contract/root 与非门禁 canary；
  `tools/ci/verify_inputs.ps1` 和 input lock 已存在，本轮未重复开发。
- 复现：读取 workflow、GitHub run/jobs 和执行 input verifier。
- 影响：真实来源、runner、工具链、源码/二进制、JUnit/退出码、artifact/源归档身份。
- 方案：在批准输入缓存和 runner 上启用 backend、windows-product、
  windows-integration-acceptance、full-quality-stress-acceptance，与 contract/root
  组成同候选六层；特权 runner 不接不可信 PR。
- 正向：六 job 同一候选和完整 artifact；负向：旧 SHA、少层、skipped、失败、缺 runner
  或授权全部阻断。现有 parser 验证结构/散列，授权真实性仍靠受控取证和批准，
  JSON 自称不是签名证明。普通 windows-2022 不被解释为正式桌面授权。
- 验收条件：同一个最终 SHA 的六层真实成功和可审核的授权/来源链。
- 本轮开始时 input verifier 为 22/22、exit 0；为验证 B13 重新构建英文引擎后，
  当前 DLL 为 `29beb9706230d45b5b26ffe77bb495e35d4b9dc4282bf4a77529ae5cf33002a6`，
  与 immutable input lock 的旧 `0e202bb8…` 不同，最终 verifier 真实 exit 1、21/22。
  没有自动重写锁来抹去漂移。需按现有“explicit maintainer action with a reviewed
  provenance change”流程复核新 engine receipt/构建日志，再更新并供应对应的缓存。
  当前本机产品/后端按新 receipt 和实际 DLL 散列构建，不将旧 cache 成功当新链通过。

### B09：安装 EXE 交付

- 原因：现有 `tools/installer/build_msi.ps1` 产出 MSI；`EpoCanva.exe` 是产品程序，
  不是安装 EXE。没有用扩展名改名来满足要求。
- 影响：Windows EXE 安装交付。
- 方案/复现：审阅 helper/MSI/record installer 脚本；后续实现固定版本工具构建的
  EXE 安装引导器，绑定内部 MSI 和完整清单；不在当前未授权宿主试装。
- 正向：真正 EXE 构建/提取/完整性；负向：payload 篡改、错误架构、失败回滚和重复
  安装；验收条件：固定候选可重建，完整安装生命周期且公开发布时最终签名。

### B10/B11：许可证、Corresponding Source、公开发行与签名

- 原因/源码：完整根 `LICENSE` 和 `docs/product/license-decision.md` 明确 AGPL-3.0-only；
  第三方源码/字典/语料各自许可，项目许可不授予其再分发权。包 source producer/
  consumer 已存在，旧归档不能为新候选背书；公开 owner/data 授权和最终签名未提供。
- 复现：`source_contract.ps1`、`stage_source.ps1`、`third_party/sources.lock.json`、
  `data/manifests/*sources.json`、package policy 与 release checker。
- 影响：内部交付技术门禁、AGPL Corresponding Source、公开再分发合法范围和签名。
- 方案：通过产品全回归后对同一包重新 stage source、逐条验证 ZIP 实际内容和依赖
  notice；固定负责人对词库/语料的分发范围决定，准备源交付可用性/适用安装信息。
- 正向：源归档逐文件散列/字节数/完整清单；负向：缺许可、遗漏字典、外层 JSON
  重新散列但内容篡改、旧包/源码/签名全部拒绝；本轮不改上游 third_party 文件。
- 验收条件：内部仍需全部技术门禁；公开另需 artifact-bound 发行/数据授权和 EXE、
  TSF、rime、installer 的发布者/时间戳/信任验证。内部免签不表示 signature_verified。

### B12：完整跨平台产品范围

- 原因：Windows 五语言组件能力不等于完整产品；无 Android IME，Spanish/German 和
  其他常驻范围语言无引擎，中文区域/多方案、日文部分确认和系统性 UI i18n 尚有缺口。
- 复现：requirements §7、`engine/locale`、backend binding 和平台源目录；不注册空
  profile、不创建固定候选假引擎。语言顺序继续调用 `LanguagePriority`。
- 影响：完整 Windows/Android 产品目标，不能在 Windows 内部轮次宣布全产品完成。
- 方案/正向：按依赖先共享 Unicode/ABI/生命周期契约，再 Android IME 离线适配与各
  语言真实方案；负向：未知 profile、安全字段、Unicode/撤权和跨平台状态一致性。
- 验收条件：每 profile 的实际引擎达到 verified 状态，跨平台真实输入和隐私测试完成。

### B13：身份计算失败必须拒绝、收据必须零跳过（已实施修复）

- 原因/真实复现：取证脚本在 caller 使用默认 Continue 时调用旧
  `Get-ProductSourceIdentity`，Windows PowerShell 未暴露 `Get-FileHash`，逐文件
  报错后仍返回一个指纹且进程 exit 0。`source-final.log` 是该无效输出的原始证据，
  **不得作为有效源码身份**。仓库 `tools/ci/verify_inputs.ps1:159` 已记录相同的
  WinPS 5.1 / pwsh 7 `PSModulePath` 自动加载问题，本轮核对了现有模块源文件。
- 源码/修复：`tools/product/build_identity.ps1` 使用直接 .NET SHA-256，可靠关闭
  stream/algorithm；身份与收据函数使用局部 Stop，逐文件摘要为空或格式错误即抛错。
  不再依赖调用方是否设置严格的错误处理。SHA 算法与行协议未改。
- 另一个问题：旧 build producer/receipt consumer 可接收 total=2/passed=1/skipped=1
  为 verified，虽然最终 release gate 还会拒绝。`build_product.ps1` 现在在发收据前
  拒绝 skip；consumer 要求真正整数计数、passed=total、failed=skipped=0。
- 正向：`build_receipt_tests.ps1` 的当前源/二进制收据、SHA-256 `abc` 已知向量、
  配置和二进制身份；负向：独占锁住输入、注入非终止读取错误、空摘要、caller
  Continue、跳过/不一致/布尔/字符串/非整数/浮点 JSON 计数、源码/配置/DLL 篡改。
  浮点负例写原始 `2.0` token，避免 ConvertTo-Json 将其规范化为合法整数 2。
- 定向正向实际 exit 0，强制失败控制实际 exit 14。开发中两次失败日志也保留：
  一次是旧 Get-FileHash 模块依赖，一次是 fixture 将 2.0 序列化为 2 的输入错误；
  后者改为真实浮点 JSON 输入，没有降低产品断言。
- 英文 engine receipt 把此控制脚本列为输入，故重新执行了真实英文引擎构建，
  没有编辑 receipt 放行。初次构建/按名称导入模块的尝试失败；最终 runner 仅在
  当前进程选择 `$PSHOME/Modules` 并加载已有 WinPS Utility 后构建成功（exit 0）。
  没有安装、修改系统模块或全局配置。完整命令和原始失败/成功日志都保留。
- 影响：源码/二进制/receipt 身份、完整测试证明；验收条件：最终全回归验证新函数，
  source-validated 与最终重算一致，并保留 B05 的 dirty/clean 区别。

## 设计选择

保留生产学习写关闭，拒绝用即时字段查询替代生命周期授权。新增独立 scalar oracle，
避免拿同一产品转换函数给自己当参考答案；复用真实 TSF 编辑层和现有 text store，
不重复实现已存在模块。修证据路径和消费端校验，拒绝删失败测试、降低阈值或改成 skip。
完整 ADR 与许可决定原地保留；新 `docs/DECISIONS.md` 仅提供索引和真实审批状态。

## 源码与文档变更清单

| 路径 | 变更 |
| --- | --- |
| `.github/workflows/windows-ci.yml` | 保留并核验初始绝对 JUnit 路径修复、陈旧文件清除与存在性校验 |
| `CMakeLists.txt` | 根工程接入完整编辑器 oracle |
| `platforms/windows/engine_bridge/CMakeLists.txt` | 产品接入同一 oracle，复用生产 edit layer |
| `tests/editor_reference_model/CMakeLists.txt` | 注册压力、最小化、强制失败、不可写报告四项测试 |
| `tests/editor_reference_model/main.cc` | 独立文档、UTF-16 选区/光标、受控 undo、异步撤权、重放/最小化与资源测量 |
| `tests/ci_gate/CMakeLists.txt` | 将真实 CTest 可执行文件传给路径集成回归 |
| `tests/ci_gate/test_workflow_evidence_paths.py` | 保留初始测试，补真实 CTest 旧路径失败/绝对路径成功和变异检查 |
| `tests/candidate_evaluation/test_candidate_policy.py` | policy/schema/批准锁/实际语料字节的正反校验 |
| `tools/backend/candidate_quality_policy.py` | 严格 schema 整数、语言容器验证与受控异常 |
| `tools/product/check_release.ps1` | 当前 Tier B 锁复核、实测阈值复算与数值严格校验 |
| `platforms/windows/product/tests/candidate_quality_gate_tests.ps1` | 未批准/伪通过/NaN/篡改等真实 parser 负例 |
| `tools/product/build_identity.ps1` | 模块无关 SHA-256、读取失败即拒绝、严格零跳过收据 |
| `tools/product/build_product.ps1` | 发 verified 收据前拒绝任意 skipped 测试 |
| `platforms/windows/product/tests/build_receipt_tests.ps1` | 摘要已知向量、独占锁/错误注入、计数类型/skip/身份篡改与 can-fail |
| `tests/windows/tsf_context_observer/CMakeLists.txt` | 注册不能被环境分类隐藏的失败负例 |
| `tests/windows/tsf_context_observer/observer_test_support.h` | 声明失败优先的环境返回接口 |
| `tests/windows/tsf_context_observer/observer_test_support.cc` | 已失败时返回 5，每线程 HWND 复用与释放 |
| `tests/windows/tsf_context_observer/observer_tests.cc` | 驱动实际焦点、禁止 fallback 冒充通知、窗口复用与原资源断言隔离 |
| `docs/DECISIONS.md` | 现行 ADR/许可/门禁索引和本轮真实未批准状态 |
| `docs/product/requirements.md` | 更正过时 CI 缺口，标注新模型能力及受控范围 |
| `docs/product/release-gate.md` | 记录批准锁/实测消费、零跳过和身份失败边界 |
| `docs/ci/windows-ci.md` | 保留 #14 失败历史、修复处置和当前六层缺口，去掉过时固定测试数/可跳过口径 |
| `docs/testing/acceptance-producers.md` | 区分旧 composer 模型、新完整文档测量与正式 entry |
| `docs/reports/acceptance-gap-matrix-20261117.md` | 需求/现状/差距/阻断对应 |
| `docs/reports/acceptance-audit-20261117.md` | 本报告、执行证据、阻断与依赖计划 |

新增提交：**无**。项目许可、真实数据审批文件和上游第三方检出未修改。
构建/取证输出逐文件列在 build 下的证据清单，不纳入源码提交。

## 执行结果

以下为本机工程证据，不是隔离安装宿主验收。完整实际 argv、cwd、时间和退出码见
`command-events.jsonl`、`root-final-r2/root-commands.json` 与
`product-final-verification/commands.json`；注册清单和 JUnit 均保存原始字节。

最终远端 HEAD / 本地 HEAD 均为 `80cb0bbdd78825036e062416137fb61a06998734`。
实际执行最终 fetch、ls-remote、`git diff --check` 均 exit 0。新提交为无。
源码内容身份为 `80cb0bbdd78825036e062416137fb61a06998734+dirty.6582fb6955c4`，
SHA-256 `6582fb6955c4a1dd653d4aee6fb3866288c0ab1ed061b84183493eb27bf59360`，562 个输入文件。
`source-validated.log` 与测试结束后的 `source-after-regression.log` 完全一致。
这是稳定的 dirty 工作区指纹，**不是已冻结的干净 Git 候选**。workflow/普通 docs
不属于产品源码指纹输入，仍逐文件纳入 `changed-files.json` 与证据清单。

| 实际执行 | 通过 / 失败 / 跳过 | 实际退出码 | 原始证据 |
| --- | --- | --- | --- |
| 根工程 Debug configure/build | 完成 | 0 / 0 | `root-final-r2/` |
| 根工程完整 CTest | 107 / 0 / 0 | 0 | `root-final-r2/root-full.xml` |
| 根完整清单/JUnit verifier | 107 项严格一致 | 0 | `root-final-r2/root-full-junit-verifier.log` |
| contract 注册清单 | 17 项严格一致 | 0 | `root-final-r2/root-contract-inventory.log` |
| 后端 Release configure/build | 完成 | 0 / 0 | `backend-configure.log`、`backend-build.log` |
| 后端完整 CTest / 清单 verifier | 61 / 0 / 0，清单一致 | 0 / 0 | `backend-full.xml`、`backend-junit-verifier.log` |
| 产品 Release configure/build | 完成 | 0 / 0 | `product-final-verification/commands.json` |
| 产品完整 CTest | 256 / 1 / 0，257 项注册/执行一致 | **8** | `product-final-verification/ctest.xml`、`ctest.log` |
| CI 证据路径回归 | 8 Python tests / 0 / 0 | 0 | `root-path-regression.log` |
| 质量策略 parser 回归 | 4 Python tests / 0 / 0 | 0 | `policy-development.log` |
| 质量消费门禁回归 | 75 断言通过 | 0 | `quality-gate-development.log` |
| 质量消费强制失败控制 | 预期失败，未当成正常通过 | 14 | `quality-negative-control.log` |
| 身份/零跳过收据回归 | 正向与负向通过 | 0 | `identity-regression-r3.log` |
| 身份强制失败控制 | 预期失败，未当成正常通过 | 14 | `identity-negative-control.log` |
| 五固定种子完整编辑器模型 | 每种 100000，共 500000 操作，0 差异 | CTest 0 | `editor-root-final/`、`editor-product-final/` |
| 模型最小化、重放、can-fail、不可写报告 | 四项 CTest 均通过；控制子进程 1/2 被真实检查 | CTest 0 | 完整 JUnit 对应 testcase 输出 |
| 英文引擎实际重建 | 完成 | 0 | `english-engine-rebuild-r3.log`、`engine-final-logs/` |
| CI 输入锁最终验证 | 21 / 22，1 个 DLL 散列不符 | **1** | `ci-inputs-final.log` |
| Tier A baseline 开发测量 | 执行完成，但质量门禁 failed | 0（baseline 执行码） | `tier-a-final/report.json`、`runs/` |
| 内部只读 release checker | acceptance_ready=false；21 总阻断，其中18内部必须 | **1** | `release-internal-final.json`、`.log` |
| 公开只读 release checker | acceptance_ready=false；23 阻断 | **1** | `release-public-final.json`、`.log` |
| 二进制/签名只读检查 | 3 文件均 NotSigned；不是验收通过 | 0（检查执行码） | `delivery-inspection.log` |

产品唯一失败项：`windows.tsf_context_observer.real_focus`，14 个检查中 3 个失败：
可见窗口关联后的真实 OnSetFocus 未到达、不能证明通知路径绑定 B、清除关联后仍发布 B。
当前未证明其根因属于测试宿主、环境还是生产实现；该项是开放工程缺陷，不能仅以外部
审批解释。原有错误分类已修，失败没有被 skip 隐藏。其余 256 项通过不关闭本阻断。

本机 `build-product-final.ps1` 外层缺少显式转发 `$LASTEXITCODE`，进程实际返回 0；
`command-events.jsonl` 忠实保留该值。内部 `build_product.ps1` 的 CTest 原始退出码
是 8，写在 commands.json，日志也显示终止后续阶段。**外层 0 不代表产品通过**。
新执行的 release wrapper 显式 `exit $LASTEXITCODE`，两种配置均真实返回 1。
产品 receipt 仍 `in_progress`，没有发放或手工补成 verified。

失败历史完整保留：#14 证据路径失败；旧质量 consumer 跑新回归有23个失败断言/exit14；
身份读取失败仍产指纹的无效 `source-final.log`；两次身份 fixture/模块失败；英文引擎
前两次模块加载失败；首轮产品2失败；最初根清单UTF-16编码造成verifier失败；Tier A
错误共享目录失败及旧DLL测量。这些均非当前成功证明，不从证据清单删除。

### 关键实际命令

以下逐项 argv 来自本轮命令记录；多词路径保留在 JSON 数组中，避免展示时丢失参数边界。
根工程与产品 configure/build/ctest 的全部 argv 另见上述各自 commands.json。

`remote-final-fetch`：真实 exit 0，日志 `remote-final-fetch.log`。

```json
["git", "fetch", "origin", "feat/internal-acceptance-round"]
```

`remote-final-head`：真实 exit 0，日志 `remote-final-head.log`。

```json
["git", "ls-remote", "origin", "refs/heads/feat/internal-acceptance-round"]
```

`backend-configure`：真实 exit 0，日志 `backend-configure.log`。

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe", "-S", "tools/backend", "-B", "build/backend-service", "-DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON"]
```

`backend-build`：真实 exit 0，日志 `backend-build.log`。

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe", "--build", "build/backend-service", "--config", "Release", "--parallel", "8"]
```

`backend-ctest`：真实 exit 0，日志 `backend-ctest.log`。

```json
["C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\ctest.exe", "--test-dir", "build/backend-service", "-C", "Release", "--no-tests=error", "--output-on-failure", "--output-junit", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\acceptance-audit-20261116\\backend-full.xml"]
```

`ci-inputs-final`：真实 exit 1，日志 `ci-inputs-final.log`。

```json
["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", "tools/ci/verify_inputs.ps1"]
```

`tier-a-development-final`：真实 exit 0，日志 `tier-a-development-final.log`。

```json
["C:\\Python314\\python.exe", "-B", "tools/backend/evaluate_candidates.py", "--cases", "data/eval/candidate-v1/cases", "--manifest", "data/eval/candidate-v1/manifest.json", "--resources", "build/product-resources/product", "--harness", "build/backend-service/candidate-evaluation-tests/Release/epocanva_candidate_eval_cli.exe", "--output", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\acceptance-audit-20261116\\tier-a-final\\report.json", "--mode", "baseline", "--runs", "2", "--repeat", "2", "--warmup-queries", "20", "--expected-repo-commit", "80cb0bbdd78825036e062416137fb61a06998734", "--rime-dll", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\librime-english\\rime\\install\\lib\\rime.dll", "--rime-dll-sha256", "29beb9706230d45b5b26ffe77bb495e35d4b9dc4282bf4a77529ae5cf33002a6", "--rime-shared", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\backend-chinese-ranked\\shared"]
```

`release-internal-final`：真实 exit 1，日志 `release-internal-final.log`。

```json
["powershell.exe", "-NoProfile", "-NonInteractive", "-File", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\acceptance-audit-20261116\\check-final-profile.ps1", "-Profile", "internal-unsigned"]
```

`release-public-final`：真实 exit 1，日志 `release-public-final.log`。

```json
["powershell.exe", "-NoProfile", "-NonInteractive", "-File", "D:\\Users\\Smart Kevin\\OneDrive\\Desktop\\EpoCanva\\build\\acceptance-audit-20261116\\check-final-profile.ps1", "-Profile", "public-signed"]
```

`source-after-regression`：真实 exit 0，日志 `source-after-regression.log`。

```json
["powershell.exe", "-NoProfile", "-NonInteractive", "-File", "build/acceptance-audit-20261116/freeze-source.ps1"]
```

`diff-check-final`：真实 exit 0，日志 `diff-check-final.log`。

```json
["git", "diff", "--check"]
```

### 开发质量与性能测量（非正式 Tier B）

固定真实 Tier A 语料15条，其中 zhuyin 1条方案未验证；实测14条，不能说15条均完成候选
质量测量。五语言最低样本要求共4504条，目前不满足。2个新进程、每例2次、20次warmup；
绑定新英文 DLL `29beb9706230d45b5b26ffe77bb495e35d4b9dc4282bf4a77529ae5cf33002a6`。
所有实测语言原文存在率/首位率均1，错误语言率0，进程内与跨进程确定性均1；这些结果
仅适用于该小样本。baseline exit0表示完成测量，report `quality_gate.status=failed`。

| 语言 | 实测例数 | 原排序 Top1/Top3/MRR | 建议 Top1/Top3/MRR（排除 raw） | warm P95 ms | 峰值 working set bytes |
| --- | --- | --- | --- | --- | --- |
| en | 2 | 0.000000/0.000000/0.141667 | 0.000000/0.000000/0.170455 | 5.4214 | 427978752 |
| zh | 3 | 0.000000/0.000000/0.177778 | 0.000000/0.000000/0.216667 | 1.7979 | 432291840 |
| fr | 3 | 0.333333/1.000000/0.666667 | 1.000000/1.000000/1.000000 | 465.5725 | 428032000 |
| ja | 4 | 0.000000/1.000000/0.500000 | 1.000000/1.000000/1.000000 | 3.8591 | 428216320 |
| ko | 2 | 0.500000/1.000000/0.750000 | 1.000000/1.000000/1.000000 | 0.1077 | 427991040 |

没有批准的绝对质量/性能标准，也没有可比性能基线，不能作合格判定；未对 Tier A 调参
制造通过。report 的 cold 字段实际为 warmup 后首测，不能称冷启动。
原文保护、建议质量、错误语言、确定性和性能已分别记录，正式 Tier B 仍 not_approved，
数值阈值仍 pending_product_owner。

### 未执行与未通过表

| 工作 | 状态 | 原因/依赖 |
| --- | --- | --- |
| 真实焦点通知和清关联 | FAILED / OPEN | 本机可复现，根因仍需继续定位；不是审批可豁免事项 |
| 干净候选 SHA 与该 SHA 新远端 CI | NOT_RUN / BLOCKED | 当前修复未提交；没有明确 commit/push 授权 |
| 当前包与 Corresponding Source staging | NOT_RUN | 最终产品回归失败、无 verified receipt；不沿用旧包 |
| 正式 stress-reference-model entry | NOT_RUN | 已有原始模型测量，缺当前候选包/JUnit/runner绑定 entry |
| Tier B qualification 与正式质量/绝对性能 | BLOCKED | 数据未批准、阈值待负责人决定；Tier A不替代 |
| 系统安装/注册、升级/修复/卸载/回滚/多用户 | NOT_RUN | 无获授权隔离桌面，未对当前宿主执行这些动作 |
| 五语言真实输入、英文跨应用、敏感字段和安装后隐私 | NOT_RUN | 缺当前合格安装包及授权真实宿主；组件测试不替代 |
| 至少1800秒真实交互浸泡 | NOT_RUN | 缺授权宿主与候选；没有伪造持续时间或退出码 |
| 六层同SHA可信CI | BLOCKED | 当前只启用contract/root/canary；输入cache散列漂移及正式runner缺失 |
| Windows安装EXE | NOT_IMPLEMENTED | 产品EXE只是壳，现有MSI脚本不能冒充EXE安装交付 |
| 产品持久学习 | DISABLED | 排队、worker与落盘授权/撤销链尚无完整证明，Unknown拒绝 |
| 公开再分发与最终签名 | BLOCKED | artifact-bound授权、数据权利确认与最终签名均缺 |
| Android、区域/方案、西/德等完整范围 | NOT_COMPLETE | 见B12，不能以语言身份注册当引擎实现 |

### 当前门禁逐项结果

下表直接来自最终内部 checker。内部有18个必须技术阻断；3项 distribution 阻断另列，
不能将内部免签表述成公开授权。公开配置还增加 acceptance-profile 和 signing 阻断。

| 门禁 | 类别 | 实际阻断原因 |
| --- | --- | --- |
| `verified-build` | technical | Product build receipt is not a verified non-trace Release build. |
| `engine-identity` | technical | Product receipt does not match the verified English engine source. |
| `build-tests` | technical | No valid build receipt; build/test evidence is unavailable. |
| `source-clean` | technical | No verified build receipt exists; a fixture package cannot establish the clean source revision. |
| `package-identity` | technical | Verified manifest/source/build identity is unavailable. |
| `public-package` | distribution | No format-2 public-distribution candidate; internal-only scope does not authorize public release. |
| `release-evidence-binding` | technical | Evidence cannot be accepted without verified source/package/build identities. |
| `corresponding-source` | technical | Corresponding Source requires current evidence/source/package/build identities. |
| `candidate-quality` | technical | Candidate quality requires current evidence, source and package-manifest identities. |
| `installer-artifact` | technical | No hash-bound distributable installer artifact or lifecycle evidence. |
| `project-license` | distribution | Missing current, hash-bound evidence: project-license. Not executed/approved is blocked, never green. |
| `lexicon-redistribution` | distribution | Missing current, hash-bound evidence: lexicon-redistribution. Not executed/approved is blocked, never green. |
| `package-diagnose` | technical | Missing current, hash-bound evidence: package-diagnose. Not executed/approved is blocked, never green. |
| `installer-lifecycle` | technical | Missing current, hash-bound evidence: installer-lifecycle. Not executed/approved is blocked, never green. |
| `real-host` | technical | Missing current, hash-bound evidence: real-host. Not executed/approved is blocked, never green. |
| `english-capabilities` | technical | Missing current, hash-bound evidence: english-capabilities. Not executed/approved is blocked, never green. |
| `offline-privacy` | technical | Missing current, hash-bound evidence: offline-privacy. Not executed/approved is blocked, never green. |
| `language-acceptance` | technical | Missing current, hash-bound evidence: language-acceptance. Not executed/approved is blocked, never green. |
| `stress-reference-model` | technical | Missing current, hash-bound evidence: stress-reference-model. Not executed/approved is blocked, never green. |
| `host-soak` | technical | Missing current, hash-bound evidence: host-soak. Not executed/approved is blocked, never green. |
| `acceptance-ci-provenance` | technical | Missing current, hash-bound evidence: acceptance-ci-provenance. Not executed/approved is blocked, never green. |

内部：acceptance_ready=false、technical_ready=false、distribution_authorized=false、
signature_verified=false、signing_status=not_required、release_ready=false。
公开：上述布尔值均false，signing_status=blocked。**技术 NO-GO / 公开发行 NO-GO；
总体 BLOCKED_NOT_COMPLETE，不能声明 INTERNAL_ACCEPTANCE_COMPLETE。**

### 二进制与证据散列

实际 EXE SHA-256 `f554ddf401f54aa523b776b500d69210bdde7ef7589cbcb7c14a6e11798ec4b4`；
TSF DLL `45f5426ce98192c81b05e74483c4b3fe6ec28ce5360cae9c9ee9625456416d13`；
英文 rime DLL `29beb9706230d45b5b26ffe77bb495e35d4b9dc4282bf4a77529ae5cf33002a6`。
三者只读 Authenticode 结果均 NotSigned。引擎源码输入身份为
`887c751a30b2231c6df1f300ce89877a35ba8ddb4b542cd033348a52c5eaf167`。
当前CI输入锁仍为旧DLL，已如实失败；没有自行改锁抹掉漂移。

| 证据（相对证据根） | SHA-256 |
| --- | --- |
| `github-root-14.log` | `1902a56b3d848cd93a3d58c56e24b1b241c23dc1f2640e575727c9f17aded864` |
| `github-root-artifact-14.zip` | `a900b765e9d09a9d23ffbb7881121bb812a050c6549234d9141ef627e935ce91` |
| `root-final-r2/root-full.xml` | `ca49b009a162b7a8188bd71b4429d9d67b5f13bf843a7f2d894aa5cde6250cd9` |
| `backend-full.xml` | `b7c00f80c263a55c19b8016a7e1dabb1197c44abe921e13788316e51663311c3` |
| `product-final-verification/ctest.xml` | `00d9426b6ed13d378bfb7b83ed2bbdd4ec71f686c70700494c55aa98430c6f04` |
| `tier-a-final/report.json` | `005b7a27ddf73d3c9f8d82002f3bdec5ba40d3788d4416affb7bed59df97851f` |
| `release-internal-final.json` | `707fc3d129b2df1a90f1f896682f1c1afb73b87d76e8bec36225b5b6ffd8c904` |
| `release-public-final.json` | `53bc43a0684ce44629792c6278dac29ce7815c86fbfc7f9d6cc6368d2980096b` |
| `source-after-regression.log` | `1d1b3b921487faa8231e58a60f234cd6547492d1b0130215f51ca1087f6dff3a` |

完整清单为 `evidence-manifest.json`，每个条目含原始文件路径、字节数和SHA-256，
包括历史失败、CI原文、最终JUnit/清单/命令、引擎receipt、模型replay、当前二进制
及25个修改源码/文档的快照。`evidence-manifest.sha256` 给出清单自身摘要；清单
明确排除自身及该摘要文件，避免循环散列。`audit-result.json` 提供机器可读结论，
`changed-files.json` 和 `final-working-tree.patch` 提供可审阅的实际变更。
这些是可核对的本机完整性记录，不是签名的远端CI或隔离宿主证明。

## 下一阶段依赖顺序

1. 继续定位真实 TSF 通知缺失与清关联后旧 target；本轮已修失败分类和窗口资源问题，
   根107/107、后端61/61已通过，产品256/257仍失败。修复后重新完整回归且零跳过。
2. 审定并提交工作区修复，固定干净 SHA；在同一 SHA 重跑远端 root 和产品层，重新
   stage package/Corresponding Source、绑定正式 editor stress entry。
3. 并行准备批准的 Tier B/绝对质量性能策略和受控 runner 输入/授权；未批准保持阻断。
4. 实现和验证安装 EXE；在获授权隔离桌面完成安装全生命周期、五语言/英文跨应用、
   敏感字段与隐私检查、至少 1800 秒实际交互浸泡；完善同 SHA 六层 CI。
5. 所有必须门禁齐全后运行固定候选 `check_release.ps1`，只有真实 acceptance_ready=true
   才可声明 Windows 内部验收完成。
6. 继续中文区域/方案、日文编辑、Spanish/German 与常驻世界语言，再共享核心/Android
   IME 适配和完整跨平台验收；持久学习须先完成 B04 生命周期证明。
7. 公开发行单独取得数据/词库与 artifact-bound 发行批准，准备 AGPL 源交付并完成最终
   签名/时间戳/信任门禁，再作公开 GO/NO-GO。
