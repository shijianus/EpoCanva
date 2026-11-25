# GO 冲刺 CI 独立审查与输入恢复（2026-11-25）

## 1. 范围、环境与最终判定

仅在 `D:/EpoGO` 既有 `codex/go-sprint-20261123` 分支工作；初始及本轮
HEAD 为 `3401bf40c8701032fcf08cb8b886f2d17b50a71e`。本轮未 commit、换分支、
push、触发 CI、安装、注册 TSF、使用签名材料或更改系统/Git 配置。
主链独立维护产品/安装/TSF；用户明确扩大授权后，仅改
`tools/product/release_verifiers/host_report.ps1` 的 `acceptance-ci-provenance`
分支。没有修改 stress/host-soak 分支或安装器测试文件。

环境：Windows x64，Python 3.14；Git 2.52.0.windows.1；Windows PowerShell 5.1
与 pwsh 7；实际 CMake/CTest 3.31.6-msvc6；MSVC 19.44.35209.0；SDK
10.0.26100.0。工具绝对路径仅在 build 原始证据中，未硬编码进正式脚本。
VS `-latest -requires ... -find ...` 探针返回空，不将其视为工具不存在：根据
真实 CMakeCache 取得并核实已安装 CTest 路径后执行。未安装任何工具。

- Overall：`ENGINEERING_FIXED_LOCALLY / REMOTE_ACCEPTANCE_BLOCKED`。
- Windows internal technical：NO-GO；本轮不宣称 acceptance_ready/technical_ready。
- Windows public release：NO-GO；未获得公开发行/数据/签名授权。
- Full cross-platform product：NO-GO；本工作范围不验收 Android/全语言。
- release_ready：false；最终值须由主链固定候选的真实 checker 给出。

## 2. 继承成果与本轮实际修改

审查前封存来自 `build/go-audit-20261125/initial-source-snapshot` 和
`initial-working-tree.patch`。旧修复没有被覆盖。下列未进一步修改的继承成果仍保留：
`tools/ci/check_whitespace.py`、`tools/ci/verify_root_junit.py`、
`tests/ci_gate/test_check_whitespace.py`、`tests/ci_gate/test_verify_root_junit.py`。
前者已支持 dispatch 范围，后者已有严格 JSON/JUnit、计数和 skip 拒绝。
`run_acceptance_layer.ps1`、workflow、输入锁验证和路径测试则在继承基础上继续修复。

本轮新增/修改的正式文件（全部未提交，commit SHA 为 N/A）：

| 文件 | 本轮内容 | 验证 |
|---|---|---|
| `.github/workflows/windows-ci.yml` | 保留六层串行与受保护 dispatch；增加唯一自托管 label，hosted JUnit producer、完整 artifact 收集与 run/attempt 命名 | 路径/负例与本地 parser；远端 NOT_RUN |
| `tools/ci/record_hosted_tests.ps1` | 实际 inventory、CTest/JUnit、退出码、源码/工作流/锁绑定，失败保留 summary | real CTest 路径回归；脚本语法双 shell |
| `tools/ci/run_acceptance_layer.ps1` | 前驱原始证据重验，修复 readiness 文件名，固定每层快照，产品/EXE/源码包绑定、实际 installer contract 执行，输入/provenance 前置 | consumer fixtures、语法双 shell；正式 self-host run NOT_RUN |
| `tools/ci/collect_layer_artifact.ps1` | 只复制散列绑定引用及前驱，验证复制结果，保留原始 JUnit/日志/产物 | 语法双 shell；远端上传 NOT_RUN |
| `tools/ci/ci_provenance.ps1` | 严格 API IDs、同 run/attempt/SHA、runner/group/labels、job/step、GitHub artifact digest、ZIP summary、真实 JUnit/退出码/产品绑定 | 正反 synthetic consumer tests，PS5.1/pwsh7 |
| `tools/product/release_verifiers/host_report.ps1` | CI 分支调用独立 consumer，其余分支未动 | 实际函数入口 synthetic tests |
| `tools/ci/verify_inputs.ps1` | 复用严格 JSON；精确字段/type/owner；目录不能冒充文件；拒绝 Git 隐藏索引 flags | 双 shell 各36项；真实22/22 |
| `tools/ci/verify_engine_provenance.ps1` | 重开所有证据，核对当前引擎源码、收据、构建退出码和 DLL | workspace pwsh7 / cache PS5.1 exit0 |
| `tools/ci/provenance/english-engine-20261125.json` | 受审查英文输入更新来源记录，保留旧 pin、实际源码/工具/PE/回归证据 | 独立 review 与消费者复验 |
| `tools/ci/inputs.lock.json` | 仅英文 DLL 散列与 provenance 文本合法更新，其余21项不变 | workspace/cache 各22/22 |
| `tools/ci/preserve_git_environment.py` | 在 CTest 前无损转换 Git process config，保留空值、顺序与安全配置；缺失即拒绝 | 实际 Git 等价与连续 CTest 3项测试 |
| `tests/ci_gate/ci_provenance_driver.ps1` | 调用实际 release CI 分支；所有文件引用重开校验 | 双 shell |
| `tests/ci_gate/test_ci_provenance.py` | synthetic 完整链及旧 SHA/run/attempt、类型、runner、artifact、JUnit、退出码负例 | 3 unittest 方法，多组 subtests |
| `tests/ci_gate/test_git_environment.py` | 原环境/转换后实际 Git 结果相等、连续 CTest、原始 exit8传播 | 3/3 |
| `tests/ci_gate/test_workflow_evidence_paths.py` | 路径规则跟随共享 producer，保留实际相对路径失败重现 | 9/9 |
| `tests/ci_inputs_lock/test_inputs_lock.py` | 新增重复/非有限JSON、锁文件、目录、隐藏Git状态负例；支持选择shell | 双 shell 各36/36 |
| `docs/ci/windows-ci.md` | 用现有真实工作流和版本化契约替换过时“design-only”及重复示例；写明运行条件、来源恢复、信任边界 | 人工对照实际代码 |
| `docs/reports/go-audit-ci-20261125.md` | 本报告 | 原始证据引用 |

`scoped-files.json` 提供交接时路径、字节数、SHA-256 与封存对照。快照只保存了
原未提交文件，因此“封存不存在”不等于本轮修改，以上表才是本轮归属清单。
测试开始时新增 ps1 为 LF；用户要求由主链冻结前统一按 .gitattributes 转 CRLF。
这些 CI 文件不在英文引擎源码 fingerprint 范围，不会要求重建已复核的引擎。

## 3. 精确缺陷/根因矩阵

| ID/级别 | 文件或路径、根因 | 修复/验证 | 剩余 |
|---|---|---|---|
| CI-01 P0 | host_report CI分支将ID强转long；仅看job name/head/status/runner_name，artifact/authorization只散列 | 严格整数；run/attempt/workflow/runner/group/labels；API artifact digest与ZIP真实summary；假类型/旧身份均拒绝 | JSON一致性不能证明授权或API检索真实性 |
| CI-02 P0 | run_acceptance_layer 前驱只读自报passed；原始JUnit和退出码可缺失 | 重开每项hash引用、inventory/JUnit、必需命令退出、层级gate快照、固定产品文件 | 可信runner执行仍必需 |
| CI-03 P1 | 读取不存在的 release-readiness-internal-unsigned.json；checker实际写 release-readiness.json | 改成实际路径并保存每层快照，避免后续重写破坏前驱 | 正式层未实跑 |
| CI-04 P1 | contract无成功JUnit；正式层只上传摘要；same needs无法固定runner | shared producer/完整绑定artifact、唯一label+group+实际runner交叉检查 | group/environment必须另行配置授权 |
| CI-05 P0 | verify_inputs ConvertFrom-Json丢重复键；Git status无法发现assume-unchanged/skip-worktree篡改 | 严格wire parser、完整schema、真实隐藏索引负例拒绝 | 文件系统并发修改仍靠受保护snapshot与前后复验 |
| CI-06 P0 | 新英文DLL与旧锁不符，旧receipt in_progress | 不直接改hash：重建日志/receipt/source/PE/exports/39测试复核后provenance+新cache | 最终候选完整回归未由本轮代跑 |
| CI-07 P0 | CTest 3.31.6在带ENVIRONMENT测试后恢复环境时丢空值，后续Git报missing GIT_CONFIG_VALUE_0 | 最小连续3测试：第一present、后两missing；在CTest前完整无损传输配置 | 已损坏父环境须重新从完整进程启动，禁止猜空值 |
| CI-08 P1 | installer unittest被hosted discovery载入，没有build_dir | 用户主链迁至tests/installer；本轮正式product job在构建后显式执行并消费JUnit | 18项实际包测试由安装/主链执行 |

Git 配置顺序依据已保存的 Git-for-Windows v2.52.0.windows.1 `config.c`：
659 行起解析单引号key/value；711 行起先读 indexed COUNT/KEY/VALUE，762 行再读
PARAMETERS。wrapper按此顺序无损编码；不读取/打印凭据，不删除安全配置、不写
系统Git配置，也不将值放进argv。真实Git前后对照覆盖空值、引号、反斜线、换行、
多值顺序。当前 CTest 环境 bug 由实际最小轨迹证明，不凭对PowerShell的推测归因。

## 4. 英文输入来源恢复与缓存

旧缓存 `build/go-input-lock-archive` 在修改锁前真实22/22 exit0，证明旧字节可取得；
它没有被覆盖。初始当前receipt为in_progress；主链两轮pwsh构建成功后，本轮
独立调用 `Assert-ProductEngineReceipt` 和 `Get-ProductEngineIdentity`，另取工具散列、
PE headers/exports/dependents、四静态库和五依赖HEAD，检查隐藏index flags。

- 当前引擎源指纹：`7e8273413bcccc1c9487fa70ed99a3a128b42f156d737f147f138cfa25c7051f`。
- DLL：`fcd61cc5005517a6a57b893e9e73c8ca8ec38f08dd02ab5d41536ed8687437a9`，2,833,920 bytes。
- 上游 librime：`33e78140250125871856cdc5b42ddc6a5fcd3cd4`；四实际依赖锁匹配且干净。
- 实测 PE x64，571 个导出名与 baseline完全相同，含rime_get_api；静态导入仅
  KERNEL32.dll/USER32.dll。此静态检查**不等于**运行期离线证明。
- 新锁 SHA-256：`3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3`。
- 缓存：`build/go-input-cache-3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3/`。
- 来源参考证据：`build/ci-input-provenance/english-engine-20261125/`，包含原始构建
  command/log、native logs、源码zip与逐项指纹、工具/库记录、实际English JUnit。
- workspace和cache均22/22 exit0；cache PS5.1 / workspace pwsh7 provenance均exit0。
  cache文件标记只读后再次22/22 exit0；不是不可撤销ACL，也不冒充授权远端cache。

最后一轮engine构建明确记录“0 objects recompiled”，所以是增量成功，不是clean
reproducibility证明。独立英文 harness重新配置/编译并执行39项，完整注册=执行，
零失败/错误/跳过，CTest exit0，独立JUnit verifier exit0。未启用pilot的39项仅是
该harness覆盖；主链另有pilot与最终根/后端/产品回归，不混为完整英文产品GO。

## 5. 实际验证与失败保留

| 验证 | 结果 | 原始证据 |
|---|---|---|
| CI工具（不含安装包dependent suite） | 33/33、exit0，首次冻结前版本 | ci-gate-final.log |
| CI provenance实际consumer | 3方法、多组负例、PS5.1/pwsh7均exit0 | ci-provenance-ps51-r2.log / ci-provenance-pwsh7.log |
| input lock fixtures | 36/36、PS5.1/pwsh7均exit0、零skip | input-tests-ps51-r2.log / input-tests-pwsh7.log |
| JUnit绝对路径实际CTest | 9/9、exit0 | hosted-path-tests-final.log |
| Git安全配置传输 | 3/3、exit0 | git-env-wrapper-tests-r2.log |
| 英文39项Release | 39注册/39过/0失败/0跳过、exit0 | english-inventory.log / english-tests.xml / english-ctest.log |
| 真实锁+cache+provenance | 全部exit0，22/22锁 | workspace-lock-final.log / cache-lock-readonly.log / *provenance-final.log |
| PS语法（本轮首次冻结） | PS5.1/pwsh7 exit0 | parse-ci-ps51.log / parse-ci-pwsh7.log |

最初锁文件测试将独占锁预期误写UNRESOLVABLE，实际稳定返回UNREADABLE，已修正
断言为真实读取失败分类；首次1失败保留。初次路径测试因本地vswhere返回空没有提供
CTest，失败保留；核实实际Cache路径后9/9。原根工程108项=105过3失败是主链真实
记录，保留；本轮不改写该结果。第一次三项定向复测发生在wrapper/path修复和
installer移动期间，exit8保留，不作为最终通过证据。最终定向复测结果见本节追加。

## 6. CI可信链与未关闭阻断

`build/go-audit-20261125/github-runners.json`为主链实际HTTP200原文：
`{ "total_count": 0, "runners": [] }`。没有本轮CI run URL、成功六层job、远端artifact
ID或授权isolated宿主可报告。未触发CI。所有synthetic fixture显式NOT RELEASE
EVIDENCE；授权JSON即使散列正确也不验证签发者身份。

| Blocker | 工程/外部 | 当前证据、下一步及完成条件 |
|---|---|---|
| CI-RUNNER-ABSENT | 外部资源/授权 | 可见仓库runner=0；负责人配置受保护group/environment/唯一label、恢复快照和真实应用；独立授权记录可核 |
| CI-CANDIDATE-UNFROZEN | 主链工程 | 当前dirty未提交；统一CRLF、源码freeze、三套完整回归后由主链创建clean SHA |
| CI-REMOTE-NOT-RUN | 工程+上述外部条件 | 同SHA六层同attempt完成success、零skip/cancel/missing；下载完整run/jobs/artifacts并验证归属 |
| CI-PLAN-APPROVAL | 外部审批/测试资源 | reviewed independent plan、真实安装生命周期、五语言/宿主/1800s交互、批准TierB/阈值仍必需 |
| CI-TOOLCHAIN-SCOPE | 工程/部署 | 22项锁不是全工具链cache；安装工具/Boost/静态库等需按各正式producer要求预置并核验 |
| CI-EVIDENCE-AUTHORITY | 信任边界 | API/approval JSON结构与散列不认证真实来源；需受控检索与真实负责人审查，不可靠自签字段关闭 |

最短依赖：本轮可信本地输入恢复完成；主链统一源字节、串行回归可立即推进。
runner/plan/数据审批可并行准备。最终commit之后再冻结package/source/receipt；之后
同候选串行六层和授权宿主/质量验收；最后真实release checker决定internal GO。
public批准/签名与Android/完整语言范围仍分别阻断对应GO，不由CI改checker代替。

## 7. 原始命令与观察退出码

以下为 `build/go-ci-20261125/commands.jsonl` 的实际记录（argv用JSON避免shell转义
歧义），没有把未执行步骤列为通过。前置只读源码审查通过工具输出记录；所有
构建/测试/输入恢复执行记录均保存在该目录。

- `cache-before` — exit `0`；`cache-before.log`

```json
["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/ci/verify_inputs.ps1", "-RepoRoot", "D:/EpoGO/build/go-input-lock-archive", "-Lock", "D:/EpoGO/tools/ci/inputs.lock.json"]
```

- `input-tests-ps51` — exit `1`；`input-tests-ps51.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_inputs_lock", "-p", "test_inputs_lock.py", "-v"]
```

- `ci-provenance-ps51` — exit `0`；`ci-provenance-ps51.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_ci_provenance.py", "-v"]
```

- `hosted-path-tests` — exit `1`；`hosted-path-tests.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_workflow_evidence_paths.py", "-v"]
```

- `hosted-path-tests-r2` — exit `1`；`hosted-path-tests-r2.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_workflow_evidence_paths.py", "-v"]
```

- `engine-independent-review` — exit `0`；`engine-independent-review.log`

```json
["pwsh", "-NoProfile", "-File", "build/go-ci-20261125/review_engine.ps1"]
```

- `hosted-path-tests-r3` — exit `0`；`hosted-path-tests-r3.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_workflow_evidence_paths.py", "-v"]
```

- `ci-provenance-ps51-r2` — exit `0`；`ci-provenance-ps51-r2.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_ci_provenance.py", "-v"]
```

- `english-configure` — exit `0`；`english-configure.log`

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe", "-S", "tools/rime_english_recovery", "-B", "build/go-ci-20261125/english-tests", "-G", "Visual Studio 17 2022", "-A", "x64", "-DCMAKE_CONFIGURATION_TYPES=Release", "-DCMAKE_GENERATOR_INSTANCE=C:/Program Files/Microsoft Visual Studio/2022/Professional", "-DRime_Install_Prefix=D:/EpoGO/build/librime-english/rime/install", "-DBaseline_Rime_Prefix=D:/EpoGO/build/librime-baseline/rime/install"]
```

- `english-build` — exit `0`；`english-build.log`

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe", "--build", "build/go-ci-20261125/english-tests", "--config", "Release", "--parallel", "4"]
```

- `english-inventory` — exit `0`；`english-inventory.log`

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe", "--test-dir", "build/go-ci-20261125/english-tests", "-C", "Release", "--show-only=json-v1"]
```

- `english-ctest` — exit `0`；`english-ctest.log`

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe", "--test-dir", "build/go-ci-20261125/english-tests", "-C", "Release", "--no-tests=error", "--output-on-failure", "--output-junit", "D:/EpoGO/build/go-ci-20261125/english-tests.xml"]
```

- `input-tests-ps51-r2` — exit `0`；`input-tests-ps51-r2.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_inputs_lock", "-p", "test_inputs_lock.py", "-v"]
```

- `ci-provenance-pwsh7` — exit `0`；`ci-provenance-pwsh7.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_ci_provenance.py", "-v"]
```

- `english-junit` — exit `0`；`english-junit.log`

```json
["python", "-B", "tools/ci/verify_root_junit.py", "--registered", "build/go-ci-20261125/english-inventory.log", "--junit", "build/go-ci-20261125/english-tests.xml"]
```

- `input-recovery` — exit `0`；`input-recovery.log`

```json
["python", "-B", "build/go-ci-20261125/recover_inputs.py"]
```

- `workspace-lock-final` — exit `0`；`workspace-lock-final.log`

```json
["powershell", "-NoProfile", "-File", "tools/ci/verify_inputs.ps1", "-RepoRoot", "D:/EpoGO", "-Lock", "D:/EpoGO/tools/ci/inputs.lock.json"]
```

- `cache-provenance-final` — exit `0`；`cache-provenance-final.log`

```json
["powershell", "-NoProfile", "-File", "tools/ci/verify_engine_provenance.ps1", "-RepoRoot", "D:/EpoGO", "-InputRoot", "D:/EpoGO/build/go-input-cache-3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3"]
```

- `workspace-provenance-final` — exit `0`；`workspace-provenance-final.log`

```json
["pwsh", "-NoProfile", "-File", "tools/ci/verify_engine_provenance.ps1", "-RepoRoot", "D:/EpoGO"]
```

- `cache-lock-final` — exit `0`；`cache-lock-final.log`

```json
["powershell", "-NoProfile", "-File", "tools/ci/verify_inputs.ps1", "-RepoRoot", "D:/EpoGO/build/go-input-cache-3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3", "-Lock", "D:/EpoGO/tools/ci/inputs.lock.json"]
```

- `ci-gate-final` — exit `0`；`ci-gate-final.log`

```json
["python", "-B", "build/go-ci-20261125/ci_tests.py"]
```

- `input-tests-pwsh7` — exit `0`；`input-tests-pwsh7.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_inputs_lock", "-p", "test_inputs_lock.py", "-v"]
```

- `parse-ci-ps51` — exit `0`；`parse-ci-ps51.log`

```json
["powershell", "-NoProfile", "-File", "build/go-ci-20261125/parse_ci.ps1"]
```

- `parse-ci-pwsh7` — exit `0`；`parse-ci-pwsh7.log`

```json
["pwsh", "-NoProfile", "-File", "build/go-ci-20261125/parse_ci.ps1"]
```

- `diff-check` — exit `0`；`diff-check.log`

```json
["git", "diff", "--check", "--", ".github/workflows/windows-ci.yml", "tools/ci", "tests/ci_gate", "tests/ci_inputs_lock", "docs/ci/windows-ci.md"]
```

- `snapshot-finalize` — exit `0`；`snapshot-finalize.log`

```json
["python", "-B", "build/go-ci-20261125/finalize_snapshot.py"]
```

- `cache-lock-readonly` — exit `0`；`cache-lock-readonly.log`

```json
["powershell", "-NoProfile", "-File", "tools/ci/verify_inputs.ps1", "-RepoRoot", "D:/EpoGO/build/go-input-cache-3d089a3c0b7fffce4e102e23d0e0373b4df736cda63810f1bb79471905be67c3", "-Lock", "D:/EpoGO/tools/ci/inputs.lock.json"]
```

- `env-empty-probe` — exit `0`；`env-empty-probe.log`

```json
["python", "-B", "build/go-ci-20261125/probe_empty_environment.py"]
```

- `env-empty-probe-property` — exit `0`；`env-empty-probe-property.log`

```json
["python", "-B", "build/go-ci-20261125/probe_empty_environment.py"]
```

- `env-empty-probe-sequence` — exit `0`；`env-empty-probe-sequence.log`

```json
["python", "-B", "build/go-ci-20261125/probe_empty_environment.py"]
```

- `git-env-wrapper-tests` — exit `0`；`git-env-wrapper-tests.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_git_environment.py", "-v"]
```

- `git-env-wrapper-tests-r2` — exit `0`；`git-env-wrapper-tests-r2.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_git_environment.py", "-v"]
```

- `hosted-path-tests-final` — exit `0`；`hosted-path-tests-final.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_workflow_evidence_paths.py", "-v"]
```

- `root-three-contracts-repaired` — exit `8`；`root-three-contracts-repaired.log`

```json
["python", "-B", "tools/ci/preserve_git_environment.py", "--", "C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe", "--test-dir", "build/go-root", "-C", "Debug", "-R", "^(candidate_evaluation|ci_gate|ci_inputs_lock)[.]contract$", "--no-tests=error", "--output-on-failure", "--output-junit", "D:/EpoGO/build/go-ci-20261125/root-three-contracts.xml"]
```

- `env-and-paths-final` — exit `0`；`env-and-paths-final.log`

```json
["python", "-B", "-m", "unittest", "discover", "-s", "tests/ci_gate", "-p", "test_git_environment.py", "-v"]
```

- `git-source-reference` — exit `0`；`git-source-reference.log`

```json
["python", "-B", "build/go-ci-20261125/git_source.py"]
```

- `root-three-contracts-final` — exit `0`；`root-three-contracts-final.log`

```json
["python", "-B", "tools/ci/preserve_git_environment.py", "--", "C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe", "--test-dir", "build/go-root", "-C", "Debug", "-R", "^(candidate_evaluation|ci_gate|ci_inputs_lock)[.]contract$", "--no-tests=error", "--output-on-failure", "--output-junit", "D:/EpoGO/build/go-ci-20261125/root-three-contracts-final.xml"]
```

- `root-three-inventory-final` — exit `0`；`root-three-inventory-final.log`

```json
["C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe", "--test-dir", "build/go-root", "-C", "Debug", "-R", "^(candidate_evaluation|ci_gate|ci_inputs_lock)[.]contract$", "--show-only=json-v1"]
```

- `root-three-junit-final` — exit `0`；`root-three-junit-final.log`

```json
["python", "-B", "tools/ci/verify_root_junit.py", "--registered", "build/go-ci-20261125/root-three-inventory-final.log", "--junit", "build/go-ci-20261125/root-three-contracts-final.xml"]
```

- `parse-ci-final` — exit `0`；`parse-ci-final.log`

```json
["powershell", "-NoProfile", "-File", "build/go-ci-20261125/parse_ci.ps1"]
```


## 8. 最终冻结补充（2026-11-25 15:23 Asia/Taipei）

最终源码版本的三个原失败CTests真实串行执行：3注册/3通过/0失败/0跳过，
CTest exit0；candidate_evaluation 9.87s、ci_gate 88.19s、ci_inputs_lock 102.67s。
这不是108项完整根回归的替代；主链继续三套完整回归。
`root-three-contracts-final.xml`、`root-three-inventory-final.log` 经独立
`verify_root_junit.py` 验证exact inventory一致，exit0。
最后PS5.1脚本语法检查exit0。安装器静态测试已接入正式windows-product并要求
实际退出码0和passing JUnit；它的18项真实执行不计入本轮无安装输入的root层。

源码已冻结，仅报告/证据索引收尾；没有commit/push/CI trigger。
完整命令列表已更新到本次最终输出。`evidence-manifest.json`仅作为文件索引，
原始日志、JUnit、来源快照均在磁盘，不能以索引本身替代任何原始证据。
