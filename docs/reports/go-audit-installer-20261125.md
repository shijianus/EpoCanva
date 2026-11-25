# 安装器独立审查与静态开发验证 — 2026-11-25

状态：`DEVELOPMENT_STATIC_VERIFIED`。安装器源码已经冻结，可进入主 agent 的串行全量回归。
本报告不宣布产品 GO：`acceptance_ready=false`、`technical_ready=false`、`release_ready=false`。
最终候选安装器、真实宿主生命周期及签名均为 `NOT_RUN`。

## 1. 范围、环境与基线

- 仅在 `D:/EpoGO`，分支 `codex/go-sprint-20261123`、HEAD `3401bf40c8701032fcf08cb8b886f2d17b50a71e` 工作；未 commit、换分支、push。
- 已读 AGENTS、requirements、release-gate、CI 文档、bootstrap 安装器章节、ADR 0003；检查了对应源码导出器与 helper 实际源码。
- 安装器相关 8 份既有未提交文件与 `build/go-audit-20261125/baseline.json` 的 SHA-256 全部一致，记录在 `build/go-installer-20261125/starting-source-check.json`。原封存目录没有修改。
- Windows 11 x64 10.0.26200；Windows PowerShell 5.1.26100.9818；PowerShell 7.6.9；Python 3.14.3；Git 2.52.0.windows.1。
- 使用现有 VS 2022 Professional、MSVC 14.44.35207（cl 文件版本 19.44.35209.0）、VS 自带 CMake 3.31.6-msvc6、Windows SDK 10.0.26100.0、既有 WiX 6.0.2。未安装工具、修改系统配置或使用签名密钥。
- 复用的产品 stage 来自 `build/go-installer-20261123/stage`，其产品提交仍为 `b97b45a80dbea17c8f4e72950657ac28380c04e7`，不是当前 HEAD。新包明确仅用于安装器开发验证，未修改旧包身份。

## 2. 缺陷与实际修复

| ID / 等级 | 可复现路径与根因 | 修改及验证 |
| --- | --- | --- |
| INST-01 / High | `build_msi.ps1` 原来仅比较 helper 二进制与收据自述散列；旧收据来自另一个工作区，没有 configure 退出码 | 新 helper format 3：重新核对源码集合、源码聚合散列、配置、编译器/链接器/CMake、真实 configure/build 退出码及日志。旧收据拒绝；重新实际构建。 |
| INST-02 / High | `helper/CMakeLists.txt` 默认 `/MD`，实际旧 PE 导入 MSVCP140、VCRUNTIME140、VCRUNTIME140_1，包未交付这些运行库 | helper 改为 `/MT`，配置身份绑定 `runtime=MT`；新 PE 导入表测试确认无外部 VC CRT 依赖。 |
| INST-03 / High | `Assert-InstallerBuildPath` 原只检查到 Boundary；Boundary 的父目录 junction 会漏过；PS 5.1 GetFullPath 会先删除尾点/尾空格 | 检查至磁盘根，并在规范化前拒绝尾点/尾空格、规范化后拒绝设备名/ADS。真实 junction 与路径负例通过。第一轮尾点/尾空格失败记录保留。 |
| INST-04 / High | MSI 提取前只检查 Admin sequence，没有检查 Directory/File/Component 图和目录 property override | 增加目录环/断链/越界组件、短名与长名、导出 stream 标识、x64 component 和属性覆盖检查；真实修改 MSI 数据库的负例通过。 |
| INST-05 / High | 原 `msiexec /a` 会启动 Windows Installer 服务，使用系统临时缓存；不能称为没有系统写入的静态操作 | 改为固定 WiX `msi decompile -x` 静态读取与提取，检查 MSI 表并重建安全布局；不调用 MSI action。提取后逐文件检查、运行隔离 user-data-root 的产品 diagnosis。 |
| INST-06 / Medium | `afterInstallValidate` 在事务初始化前移除旧版本，不利于失败恢复 | 改为 `afterInstallInitialize`；读取真实 MSI InstallExecuteSequence 验证 RemoveExistingProducts 位于事务内。真实升级回滚仍 NOT_RUN。 |
| INST-07 / Medium | bootstrapper 无版本资源；同机 VS Community/Professional 的发现次序使 helper 与 EXE 编译器不同 | 生成真实 VERSIONINFO；EXE ProductVersion=0.2.6-w3b、FileVersion=0.2.6.0；EXE 强制复用 helper 收据中的实际编译器。 |
| INST-08 / High | 原消费端缺少源文件、工具及日志重新核验；散列函数可能受调用者 Continue 错误策略影响 | 新 provenance 公共模块及 format 4 MSI / format 3 EXE 契约，严格整数/布尔与退出码检查，散列读取强制 Stop。文件缺失、独占锁、读取中故障、外层重算散列、源码/工具链篡改均拒绝。 |

保留既有正确实现：`bootstrapper.cc` 的 AMD64 PE32+ / overlay 边界、编译入镜像的 MSI SHA-256、文件读取租约、随机受限 ACL 临时目录、固定系统 msiexec 路径、允许参数集合、原始子进程退出码传播。`--verify` 在临时文件创建与安装调用之前返回；真实运行确认未创建临时载荷。未将散列一致性称为代码签名或可信 runner 证明。

保留 `wix_tool.ps1` 对固定 NuGet 包及解包工具树的逐文件验证。复制缓存后修改 wix.exe、加入 DLL、删除文件的负例均拒绝，没有通过更新 expected hash 消除漂移。

## 3. 本次逐文件修改

所有修改均未提交；commit SHA 为 N/A，基于上述 HEAD 和已封存未提交修改。

| 文件 | 本次修改 | 验证 |
| --- | --- | --- |
| `tools/installer/build_bootstrapper.ps1` | 版本资源、与 helper 一致的编译器、源码/工具/日志身份、EXE format 3 | 实际编译、`--verify`、双 PowerShell 契约测试 |
| `tools/installer/build_helper.ps1` | helper format 3、configure/build 日志与退出码、源码和工具链重验 | 实际 CMake configure/build、双 PowerShell 消费验证 |
| `tools/installer/build_msi.ps1` | helper 显式目录参数、严格 package format 2、MSI 版本一致、事务内升级、纯静态提取、MSI format 4 | 实际 WiX 构建、提取、66 份产品文件+manifest+helper 共 68 文件检查 |
| `tools/installer/helper/CMakeLists.txt` | 静态 CRT | 实际 PE 导入表测试 |
| `tools/installer/installer_contract.ps1` | 路径、MSI 结构、版本及各层身份消费校验、读取失败即停 | 双 PowerShell 正反测试 |
| `tools/installer/provenance.ps1`（新增） | 源码范围与身份、文件引用及 helper 收据消费校验 | 源码/工具链/配置/日志/退出码负例 |
| `tools/installer/static_msi.ps1`（新增） | WiX 纯静态提取、受控布局、真实退出码传播 | 实际提取；模拟工具 exit 23 导致外层非零 |
| `tools/installer/record_installer.ps1` | 使用清单中的 helper receipt 路径和新的严格消费验证 | 语法检查；真实最终证据写入 NOT_RUN，等待主 agent 候选 |
| `tests/ci_gate/test_installer_contract.py` | 18 项真实文件/PE/MSI/脚本测试及 JUnit 输出 | PS 5.1 与 PS 7 各 18/18 |
| `docs/reports/go-audit-installer-20261125.md`（新增） | 本报告 | 命令、证据与范围核对 |

`tools/installer/bootstrapper.cc`、`tools/installer/wix_tool.ps1` 仅审核，保持本轮开始时已有未提交内容。未改 `tools/product`。
`platforms/windows/product/tests/evidence_input_tests.ps1` 的 AST 提取仍找到唯一实际 `Assert-WixVersion`，无需修改；正例 exit 0，故意失败控制 exit 14。

## 4. 实际验证与命令

全部原始构建/测试命令、工作目录、UTC 起止时间、实际子进程退出码、日志散列见 `build/go-installer-20261125/*.command.json` 和汇总 `commands.json`。每个命令通过同目录 `run.py` 记录；下面列出其实际子命令。

| 验证 | 精确子命令（工作目录 D:/EpoGO） | 退出码 |
| --- | --- | --- |
| 最终 helper | `powershell.exe -NoProfile -NonInteractive -File tools/installer/build_helper.ps1 -BuildDir D:/EpoGO/build/go-installer-20261125` | 0（helper-r3） |
| 最终开发 MSI | `powershell.exe -NoProfile -NonInteractive -File tools/installer/build_msi.ps1 -BuildDir D:/EpoGO/build/go-installer-20261125 -HelperBuildDir D:/EpoGO/build/go-installer-20261125` | 0（msi-r3） |
| 最终开发 EXE | `powershell.exe -NoProfile -NonInteractive -File tools/installer/build_bootstrapper.ps1 -BuildDir D:/EpoGO/build/go-installer-20261125` | 0（bootstrapper-r3） |
| PS 5.1 全部契约 | `python tests/ci_gate/test_installer_contract.py --build-dir build/go-installer-20261125 --powershell powershell.exe --junit build/go-installer-20261125/contracts-final-ps51.junit.xml -v` | 0 |
| PS 7 全部契约 | `python tests/ci_gate/test_installer_contract.py --build-dir build/go-installer-20261125 --powershell pwsh.exe --junit build/go-installer-20261125/contracts-final-ps7.junit.xml -v` | 0 |
| 原有 AST 测试 | `powershell.exe -NoProfile -NonInteractive -File platforms/windows/product/tests/evidence_input_tests.ps1 -RunRoot D:/EpoGO/build/go-installer-20261125` | 0 |
| AST 故意失败控制 | 上一命令追加 `-ForceFailure` | 14 |

| 最终测试套件 | 注册/实际执行 | 通过 | 失败 | 错误 | 跳过 |
| --- | ---: | ---: | ---: | ---: | ---: |
| installer contract / PS 5.1 | 18 / 18 | 18 | 0 | 0 | 0 |
| installer contract / PS 7.6.9 | 18 / 18 | 18 | 0 | 0 | 0 |

根工程、backend、产品 Release 三套全量 CTest 不在本安装器子任务执行范围，交由主 agent 串行执行，未冒称通过。

失败历史未删除：第一轮 18 测试中路径测试的两个 subcase 失败，exit 1；修复后独立路径回归及全部测试通过。一次 bootstrapper 过早读取旧 MSI schema，exit 1，随后等待上游 MSI 完成后重跑成功。首次语法探测因 shell 参数展开失败，exit 1，改用脚本文件后 PS 5.1/7 解析通过。WiX `--help` 自身返回 4294967295，帮助文本仍保留；未将该值报告为成功。首次旧 `/a` 路径真实完成（exit 0），但发现 Windows Installer 系统临时缓存写入，日志保留，新最终流程已移除 `/a`。

## 5. 最终开发产物

- 产品版本：0.2.6-w3b；MSI 数字版本：0.2.6；文件架构：x64。
- MSI：`build/go-installer-20261125/installer/EpoCanva-0.2.6-w3b-x64.msi`，25,194,496 bytes，SHA-256 `dbf2c0b9d0686d9c40a683c242df3b92d9ce5acd68dc5860b08481a338034531`。
- EXE：`build/go-installer-20261125/installer/EpoCanva-0.2.6-w3b-x64-setup.exe`，SHA-256 `b19112c8d1814f5afa8dc8697011b0445584a9a08ff547e39157ce0bd3225aa4`。
- helper：163,328 bytes，SHA-256 `61231d024f46624f29f1b58f3645fb7f750acc9ce4f2365ff5ad1a805d0a4b59`。
- package.manifest SHA-256 `31a1fba4e907ca8695bb15b44f92cde11f62acbf756b23ad410b96bebf1c3895`，仍绑定旧产品提交；没有伪造当前 clean SHA。
- `helper-build.json`、`installer/installer-manifest.json`、`installer/bootstrapper-manifest.json` 分别为 format 3/4/3。
- evidence-manifest、changed-files、installer-only.patch、最终源码 snapshot 和 audit-result 全在 `build/go-installer-20261125`。

## 6. 最终阶段精确接入方式

主 agent 将 `$CandidateBuild` 设为**实际完成最终产品构建与 stage 的绝对 preset 目录**；它必须是 `D:/EpoGO/build/` 的直接子目录。不要指向本次旧产品开发验证包。

```powershell
# Run from D:/EpoGO after the final product stage exists.
$StageRoot = Join-Path $CandidateBuild 'stage/EpoCanva-0.2.6-w3b'
$WixPath = 'D:/EpoGO/build/_local/wix-tool/wix.exe'
powershell.exe -NoProfile -NonInteractive -File tools/installer/build_helper.ps1 -RepoRoot D:/EpoGO -BuildDir $CandidateBuild
if ($LASTEXITCODE -ne 0) { throw "helper exit $LASTEXITCODE" }
powershell.exe -NoProfile -NonInteractive -File tools/installer/build_msi.ps1 -RepoRoot D:/EpoGO -BuildDir $CandidateBuild -StageRoot $StageRoot -HelperBuildDir $CandidateBuild -WixPath $WixPath
if ($LASTEXITCODE -ne 0) { throw "MSI exit $LASTEXITCODE" }
powershell.exe -NoProfile -NonInteractive -File tools/installer/build_bootstrapper.ps1 -RepoRoot D:/EpoGO -BuildDir $CandidateBuild
if ($LASTEXITCODE -ne 0) { throw "EXE exit $LASTEXITCODE" }
python tests/ci_gate/test_installer_contract.py --build-dir $CandidateBuild --powershell powershell.exe --junit (Join-Path $CandidateBuild 'installer-contract.junit.xml') -v
if ($LASTEXITCODE -ne 0) { throw "installer tests exit $LASTEXITCODE" }
# Only after the main agent creates current-candidate release-evidence.json:
powershell.exe -NoProfile -NonInteractive -File tools/installer/record_installer.ps1 -RepoRoot D:/EpoGO -BuildDir $CandidateBuild -StageRoot $StageRoot
if ($LASTEXITCODE -ne 0) { throw "record installer exit $LASTEXITCODE" }
```

本机已验证的工具路径（仅本机事实，未写入构建脚本硬编码）：

- WiX shim：`D:/EpoGO/build/_local/wix-tool/wix.exe`。
- 经包散列验证后实际执行：`D:/EpoGO/build/_local/wix-tool/.store/wix/6.0.2/wix/6.0.2/tools/net6.0/any/wix.exe`。
- WiX nupkg：同一 `6.0.2` 目录下 `wix.6.0.2.nupkg`；SHA-256 `13caed0aa86898c9952eb8ba82c6ac6b43d1575bb731ac848e5edf5490a10428`；上游提交 `b3f340393117094a75ea8ced77f2357e4aa095e7`。
- CMake、cl、link 和 rc 的机器绝对路径保留在本地 `helper-build.json` 与 `installer/bootstrapper-manifest.json` 的 toolchain 字段，不写入版本控制的机器配置；交接回复另提供本机路径。

每个实际工具的散列及 bytes 都在收据中，bootstrapper 必须与 helper 选中同一个 cl。旧 `build/installer-tools/helper-build.json` 不符合新格式，不能直接复用；按上面命令在最终 preset 内重建即可。

## 7. 未关闭阻断及交接

| Blocker | 类型 | 当前证据 / 下一步 / 完成条件 |
| --- | --- | --- |
| INST-FINAL-CANDIDATE | 工程集成 | 当前 stage 是旧产品 SHA。主 agent 完成 clean SHA 全回归后，按上述串行命令重建 helper→MSI→EXE，再绑定最终 package 与 release evidence。 |
| INST-GATE-INTEGRATION | 工程集成（tools/product 不在本任务写范围） | 本次检查的 `tools/product/release_verifiers/installer.ps1` 仍只检查扩展名与外层 hash；需要调用 `Assert-EpoCanvaInstallerPackage`，要求真实 EXE、format 3/4/3、真实 MSI/版本/源码与证据一致，失败即门禁阻断。`record_installer.ps1` 已调用该公共消费者，但不能代替 checker 独立调用。 |
| INST-HOST-LIFECYCLE | 必需授权环境 | install/repeat/upgrade/repair/uninstall/rollback/cleanup、TSF 注册注销、多用户与真实 msiexec 退出码均 NOT_RUN。需要可恢复隔离宿主授权和真实测试；静态升级 sequence 检查不替代回滚实验。 |
| INST-HELPER-OWNERSHIP | 工程复核及授权验证 | `tools/product/isolation/tip_profile_helper.cc:260` 的 Unregister 先处理 profile 再调用 DLL ownership 检查；Register 的失败补偿也可能撤销先存注册。该共享源码不在本任务写范围，需主 agent 对既存/外来注册和失败注入做所有权回归，再在隔离环境验收。 |
| INST-TRUST | CI/供应链 | 本地 receipts 是一致性记录，不是签名或受信任执行证明。必须用可信 runner/输入锁把最终 EXE、helper、MSI、源码和工具链证据绑定到统一候选；攻击者同时替换二进制和所有自述记录不能仅靠无签名 JSON 防范。 |
| INST-SOURCE-PUBLIC | 工程及负责人批准 | `source_contract.ps1:131` 枚举 tracked 与 non-ignored untracked 的 tools，因此本次新模块属于对应源码集合；最终必须重新运行 source producer，不复用旧 ZIP。WiX 包的 `OSMFEULA.txt`/nuspec 已核读，公开使用许可条件与项目第三方清单仍需负责人核对；AGPL 不授予第三方数据再分发权。 |
| INST-SIGNING | 公开发行授权 | 未签名、未使用私钥、未验证真实时间戳/发布者信任链。需公开发行授权后的独立签名流程；当前 unsigned 静态验证不会给出 signing success。 |

没有进行实际系统输入法安装、TSF 注册、升级、卸载或签名。首次 administrative image 操作及其系统临时缓存行为已如实单列，不能表述为全过程零系统写入。最终静态链只提取文件、读取 MSI 数据库、运行隔离诊断和 `--verify`。

网络保证边界保持原政策：普通 Windows TSF DLL 的离线保证是代码/依赖架构保证，不能宣称 OS 网络隔离；Android 可通过不声明 INTERNET 权限获得平台强制，本安装器验证未验收 Android。

## 8. 核验来源与选择

- Microsoft Learn `removeexistingproducts-action`、`administrative-installation` 的实际正文已抓取到证据目录，URL 和 SHA-256 见 `official-specs.json`。前者核对事务顺序，后者明确 /a 执行 AdminUISequence/AdminExecuteSequence。
- 本机 Windows SDK `um/MsiQuery.h:32,199,382` 核对只读数据库打开与查询；`um/winver.h:146,233` 核对版本资源 API。
- `wix msi decompile --help` 与真实提取结果验证 `-x/-intermediateFolder/-o`；固定包 nuspec 核对上游 commit 与 OSMFEULA。
- 保留既有 bootstrapper，不引入新的安装框架；改静态提取而不是放宽 admin action allowlist；重建 helper 而不是修改旧收据；保留真实失败而不是改为 skip；对应源码导出器由主 agent维护，不跨写范围修改。

本轮源码冻结后最短依赖链：主 agent 全量回归 → 最终 clean 候选与 stage → helper/MSI/EXE 静态构建及本契约回归 → 新 Corresponding Source 与 checker 独立核验 → 获授权隔离宿主生命周期 → 相应技术 GO。可信 CI、许可核对和隔离环境筹备可并行；最终证据绑定必须在候选冻结后串行完成。
