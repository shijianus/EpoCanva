# 2026-11-19 独立验证轮报告（会话 sess_e700d024）

状态：**技术 NO-GO（内部验收未完成）/ 公开发行 NO-GO / 总体 BLOCKED_NOT_COMPLETE**。
本报告是只读独立验证与实测诊断轮：不授予数据、宿主、签名或发行批准；所有执行
命令与退出码为真实记录。审计起点为分支当前远端 HEAD，不是 main。

## 0. 并发会话披露（诚实性要求）

本轮执行期间检测到另一并行会话（`sess_c7d01c7e`，同一任务简报）在同一工作区
运行，并于 2026-11-19 03:22/03:33 修改了
`tests/windows/tsf_context_observer/observer_tests.cc`（+46/-3，仅新增检查，
未删除/削弱任何既有断言）。为避免互相覆盖，本会话在完成既定验证后停止修改
源码，转为独立验证者，并已通过消息向该会话通报探针事实。**本报告的测试证据
全部基于已提交的 `c567eb0` 二进制，不含其未提交改动**；其改动完成后的最终树
复核是 §7 的强制后续步骤。

## 1. 审计身份与起点

- 分支：`feat/internal-acceptance-round`；本地与远端 HEAD 均为
  `c567eb045a72c2b6787edbd3249c7ab7c220e6bc`（"bakup"，2026-11-17）。
  实际执行 `git fetch origin feat/internal-acceptance-round`（exit 0）与
  `git rev-list --left-right --count HEAD...FETCH_HEAD`（`0 0`）：
  **远端无更新，审计起点即当前 HEAD，未改用 main**。
- 已知历史：`80cb0bb`（windows-ci #14，run 43570509740，job 132374343144
  root-unit-tests **failure**，2026-11-14）的失败原因（JUnit 相对路径嵌套）、
  处置与验收条件已由 [2026-11-17 审计](acceptance-audit-20261117.md)记录，
  修复随 c567eb0 提交。该失败历史原样保留于
  [windows-ci 文档](../ci/windows-ci.md)与 2026-11-17 报告，未删除、未改写。
- 环境：Windows 11 x64（26200）、PowerShell 5.1、Python 3.14.3、Git 2.52.0、
  VS CMake/CTest 3.31.6-msvc6。`gh` CLI 不存在（探测 exit 127）；改用 GitHub
  API 匿名只读获取 run/jobs 元数据；job 日志下载 API 匿名返回 403，未绕过。
- 证据根：`build/acceptance-round-20261119/`（本报告引用的日志均在其下），
  CI 元数据原始 JSON 在 `build/evidence-tmp/`。构建产物全部在 `build/<preset>/`。

## 2. 远端 CI 现状（原始 API 证据）

| run | SHA | 结果 | jobs |
| --- | --- | --- | --- |
| 44009730131（2026-11-17） | `c567eb0` | success | contract-gate-canary、contract-gate、root-unit-tests 全 success |
| 43570509740（2026-11-14） | `80cb0bb` | failure | root-unit-tests failure（#14，历史失败保留） |

**B01 的验收条件"新干净候选 SHA 的真实远端 root job 成功"已在 `c567eb0` 满足**：
本轮开始时该 SHA 工作区干净（`git status` 为空），远端三 job 全绿。
"不用旧 SHA 成功替代当前候选"仍然成立——`c567eb0` 就是当前候选。

## 3. 本机独立复核（本轮真实执行，非引用历史报告）

| 执行 | 结果 | 实际退出码 | 原始证据 |
| --- | --- | --- | --- |
| 根 configure（`--preset vs2022-x64-debug`） | 完成 | 0 | `root-configure.log` |
| 根全目标 build（`-j8`） | 完成 | 0 | `root-build.log` |
| 根完整 CTest（Debug，JUnit=绝对路径） | **107 通过 / 0 失败 / 0 跳过** | 0 | `root-ci/ctest.xml`、`root-ci/registered.json` |
| `verify_root_junit.py`（107 项清单严格一致） | 通过 | 0 | 同上 |
| 后端 build + 完整 CTest（Release） | 全过 | 0 / 0 | `backend-build.log`、`backend-ctest.xml` |
| 产品 build + 完整 CTest（Release） | **257 通过 / 0 失败 / 0 跳过**（#114 real_focus 通过，1.49s） | 0 / 0 | `product-build.log`、`product-ctest.xml` |
| real_focus 空闲重复（`--repeat-until-fail 12`） | 12/12 通过 | 0 | `real-focus-repeat-idle.log` |
| real_focus 负载重复（并行构建压测同跑） | 12/12 通过 | 0 | `real-focus-repeat-load.log` |
| `verify_inputs.ps1`（CI 输入锁） | 21/22；librime-english DLL 散列漂移未处置 | **1** | `ci-inputs-now.log` |
| `check_release.ps1 -AcceptanceProfile internal-unsigned` | acceptance_ready=false，21 项阻断 | **1** | `release-internal-now.log` |
| `check_release.ps1 -AcceptanceProfile public-signed` | acceptance_ready=false，23 项阻断 | **1** | `release-public-now.log` |
| 编辑器参考模型（根 #89-91、产品 #203-206） | stress / replay_minimization / can_fail / report_unwritable 全过 | 0 | 对应 JUnit |
| 质量门禁回归（产品 #219-220 等） | normal / can_fail 全过 | 0 | 对应 JUnit |
| 学习写关闭源码核对 | `ProductLearningWritesPermitted()` 恒 `false`（`platforms/windows/product/product_settings.cc:295`） | — | 源码引用 |

## 4. B02 真实焦点通知：实测诊断（本轮核心工程结论）

自制诊断探针（`build/acceptance-round-20261119/probe/`，复用仓库真实
`TestHost`，标注为诊断工具、非测试、非验收证据），**10/10 次运行结果一致**：

| 焦点路径 | OnSetFocus 投递 | TSF 内部 `GetFocus` |
| --- | --- | --- |
| 程序化 `ITfThreadMgr::SetFocus(B)` | **从不投递**（0 回调） | 移动到 B |
| `AssociateFocus(win,B)` + 真实 `::SetFocus(win)` | **每次投递**（该步 2 次回调） | 移动到 B |
| 清除关联 + `::SetFocus(nullptr)` | **每次投递**（1 次回调） | 移回 |

与 Microsoft Learn 文档一致（AssociateFocus 使 TSF 在窗口获得焦点时自动调
SetFocus；OnSetFocus 随文档焦点变化投递）：**通知走窗口焦点跟踪路径；本机的
程序化 SetFocus 不产生回调是稳定行为，不是缺陷**。

**2026-11-16 审计轮的 #114 三项断言失败今天无法复现**：提交版代码空闲 12/12、
负载 12/12 全过。审计原始输出
（`build/acceptance-audit-20261116/product-final-verification/ctest.log`）显示
当时状态为：AssociateFocus 成功、窗口持有 Win32 焦点、TSF 内部 GetFocus 已移到
B（Pump 重验证发布 B，binds=2/drops=1），但 `focus_notifications=0->0`，且清关联
后内部焦点滞留——通知被会话级状态抑制。该原始证据完整保留；属 msctf 会话态，
当前环境（含负载）无法复现。

结论：生产观察器与该测试的失败关闭设计（exit 5、77 仅在无先前失败时）
经复核无缺陷；缺陷性质为**环境级瞬态**。并行会话正在对该测试做防御性加固
（绑定前清除陈旧 TSF 焦点、先聚焦窗口再 AssociateFocus），方向与探针事实一致。

## 5. 更新后的需求与实现差距矩阵（2026-11-19）

| 需求/门禁 | 本轮状态 | 依据 |
| --- | --- | --- |
| 根工程测试与证据完整性 | **B01 关闭**：c567eb0 远端 3 job 全绿 + 本机 107/0/0 零跳过双重复核 | §2、§3 |
| 固定干净候选（B05） | `c567eb0` 为当前干净候选；工作区现有一处他人并发改动未提交 | §0 |
| TSF 真实焦点通知（B02） | 无可复现缺陷；降级为开放瞬态；防御加固在途；须最终树复验 | §4 |
| 持久学习（B04） | 维持关闭（正确）；排队→worker→落盘授权/撤销链证明仍缺 | `product_settings.cc:295` |
| 编辑器参考模型（B03/需求 7） | 5×100000 测量本体通过；正式 `stress-reference-model` entry（绑定当前包/JUnit）仍缺 | CTest 证据 |
| 候选质量（B06/需求 8） | 门禁代码回归通过；Tier B 仍 `not_approved`、阈值 `pending_product_owner`（外部依赖） | DECISIONS.md |
| 安装生命周期/1800s 浸泡（B07/需求 9） | NOT_RUN（无获授权隔离桌面；外部依赖） | — |
| 六层 CI（B08/需求 10） | 仍 contract/root/canary 三层；输入锁 21/22 漂移需 maintainer 复核流程 | `ci-inputs-now.log` |
| 安装 EXE（B09/需求 11） | 未实现（产品 EXE≠安装 EXE） | — |
| AGPL/词库/签名（B10/B11） | 公开 23 项阻断维持；内部 21 项阻断维持 | release-*.log |
| 跨平台完整范围（B12） | 无变化（Android/西/德/区域方案未实现） | — |
| 身份与零跳过（B13） | c567eb0 修复经本轮 107+61+257 零跳过复核确认 | §3 |

## 6. GO/NO-GO 判定

- **Windows 内部验收：NO-GO**。`check_release.ps1` 内部档
  `acceptance_ready=false`、21 项阻断（18 技术 + 3 distribution）；
  未满足"真实 `acceptance_ready=true` 且无必须门禁阻断"的唯一声明条件。
- **公开发行：NO-GO**（23 项阻断；发行/数据授权与最终签名均缺）。
- 技术面进展：root-unit-tests 修复经远端 + 本机双重复核确认；B02 无可复现缺陷。

## 7. 强制后续步骤（按依赖排序）

1. 等待 `sess_c7d01c7e` 完成；对其工作区改动做完整审查（不许削弱断言；
   当前快照 +46/-3 仅新增检查）。
2. 最终树重建产品 Release；real_focus 空闲/负载各 ≥12 次重复 + 根/后端/产品
   全量回归（零跳过）；任何失败回到修复循环。
3. 审定后提交、固定新干净 SHA、推送并确认远端 windows-ci 三 job 绿。
4. B08 输入锁漂移按文档流程做 maintainer 复核（评审 engine receipt 与构建日志
   后更新锁并供应缓存）；不得静默改锁。
5. 外部依赖项（Tier B 数据、绝对阈值、隔离宿主授权、签名/发行授权）仍需负责人
   批准；未批前相关门禁保持 BLOCKED，不得用合成数据或降低标准替代。

## 8. 下一阶段开发计划（依赖排序）

1. **并发收敛**：并行会话改动审查 + 最终树全量回归（§7.1–7.2）→ 固定新 SHA
   + 远端 CI（§7.3）。
2. **B08**：输入锁 maintainer 复核 → 六层 CI 启用评估（需正式 runner 输入与授权）。
3. **外部批准流**（并行推进）：Tier B 数据批准 → 质量/性能绝对阈值批准 →
   隔离桌面授权（`runner_authorization`/`isolation_authorization` hash-bound 记录）。
4. **B09**：安装 EXE 引导器实现与验证（固定版本工具、payload 绑定、负例）。
5. **B07**：获授权宿主执行安装全生命周期、五语言真实输入、英文跨应用、敏感字段、
   隐私与 ≥1800 秒浸泡 → 产出 host-soak / real-host / language-acceptance 证据。
6. **正式 entry**：stress-reference-model 正式 entry（绑定当前候选/包/JUnit/
   runner/重放/资源采样）→ aggregate gate。
7. **B04**：学习授权生命周期（排队、worker、落盘、撤权）证明齐全后再评估开启。
8. **B12**：中文区域/多方案、日文、西/德等引擎 → 共享核心契约 → Android IME 适配。
9. **公开发行**：单独取得数据/词库与 artifact-bound 发行批准，AGPL 源交付与
   最终签名/时间戳/信任门禁，再作公开 GO/NO-GO。

## 9. 本轮证据文件清单（SHA-256 前缀，完整散列见 `build/acceptance-round-20261119/`）

| 证据 | 大小 | SHA-256 |
| --- | --- | --- |
| `root-ci/ctest.xml` | 63611 | `5ccd6dee8d891cd0…49cd8ea2` |
| `root-ci/registered.json` | 135000 | `9fc52c105316ad7a…8f0f8e1d` |
| `product-ctest.xml` | 262947 | `168b4054827bcec2…2ef7be68` |
| `backend-ctest.xml` | 33719 | `c66d544ec45191ed…98584a3d` |
| `real-focus-repeat-idle.log` | 2263 | `62bd0251d8fed3b2…07e7db74` |
| `real-focus-repeat-load.log` | 2263 | `433d9c6d46ba5eb6…8d00416f` |
| `ci-inputs-now.log` | 1426 | `d24d0c9ed4345b68…0aa31b7f` |
| `release-internal-now.log` | 2623 | `0c5ae38627e26c80…aa3f21a3` |
| `release-public-now.log` | 2827 | `bdfaa991b90f0494…5308434a` |
| `probe/focus_probe.cc` | 12364 | `c840e4aa8fe913b2…bedebad9` |
| `probe/run1.log` | 1427 | `facd2851748b906a…46a46f5d` |

## 10. 本会话源码变更

**无**（诚实记录）：本会话的全部修复工作为验证、探针与报告；源码修改由并行
会话进行中，本会话为避免竞争未触碰源码树（本文件为唯一新增文档，不属于产品
源码指纹输入）。`c567eb0` 仍是最近已验证的干净候选。
