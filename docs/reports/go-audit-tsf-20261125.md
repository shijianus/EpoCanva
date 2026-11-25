# TSF 独立复核与交接 — 2026-11-25

## 1. 最终状态与边界

**BLOCKED_NOT_COMPLETE / TSF P0 尚未全部满足。** 所负责源码已冻结，允许主链开始串行全量回归。
Windows internal technical / public release / full cross-platform GO 均不能由本轮宣布；
`acceptance_ready=false`、`technical_ready=false`、`release_ready=false`。

仅在 `D:/EpoGO`、`codex/go-sprint-20261123` 工作。HEAD 始终为
`3401bf40c8701032fcf08cb8b886f2d17b50a71e`；**无 commit、push、分支创建或系统注册/安装**。
所有改动未提交，不能把 HEAD 当成当前工作区内容身份。
`build/go-tsf-20261125/source-freeze.json` 绑定逐文件 SHA-256；
`final-source/` 保存源码本体，`this-round-source.patch` 仅含相对接手时的增量。
全量根工程、backend、产品 Release 及最终候选/CI 归主链，本轮没有执行或宣称这些全量通过。

环境：Windows x64，CMake 检测目标 10.0.26200，SDK 10.0.26100.0，VS 2022 Professional，
MSVC 19.44.35209.0，CMake/CTest 3.31.6 系列，Release、C++17、扩展关闭、静态 CRT、
`/permissive-`。CMake 路径从现有 `build/go-tsf-20261123/CMakeCache.txt` 取得；未安装工具。
构建和测试产物均在 `build/go-tsf-20261125/`。

已查阅 AGENTS、产品 requirements、context-observer/focus-admission/TSF edit/engine-lifetime
合同，以及 ADR 0002、0003、0007、0011。保留隐私、离线、单位和授权边界。
Windows TSF DLL 的离线保证是代码及依赖无网络调用的工程保证，不能声称宿主进程受到 OS 网络隔离；
Android 的网络限制可通过不声明 INTERNET 权限平台强制落实。本轮未测试 Android。

## 2. 继承成果与本轮新增必须区分

接手时，指定源码范围中 9 个已有修改文件的 SHA-256 **全部与主链封存 baseline 匹配**。
记录在 `inherited-baseline.json`，源码保存在 `inherited-source/`。
没有 SOURCE_TRANSFER_BLOCKED，也没有覆盖或重做继承实现。

继承并保留的成果包括：真实三窗口焦点驱动、AssociateFocus 原关联恢复、
真实通知/Pump 分开计数、owner thread/apartment/advise/unadvise 快照、Shutdown 撤销 target、
published generation 检查、weak write guard、失效 set 通知不得覆盖失败/false 实测查询、
association/revoked guard 负例，以及取消 exit 77 的门禁。
`target_revalidation.cc/.h` 本轮**未改动**，最终散列与继承快照相同。

## 3. 原始规范与源码核对

官方页面正文副本均留在证据目录，以散列登记；不是把既有报告当成规范：

| 接口 | 已核对的合同与定位 |
| --- | --- |
| [AssociateFocus](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfthreadmgr-associatefocus) | 保存副本第 63/67 行：new 可 NULL、管理器不替调用者 AddRef，previous 为返回引用；104/108 行：关联窗口**获得焦点时**自动 SetFocus，恢复时重用 original previous。不能推导“解除关联立即失焦”。 |
| [SetFocus](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfthreadmgr-setfocus) | 保存副本第 59 行明确参数不能 NULL；SDK `um/msctf.h:1093` 的 `__RPC__in_opt` 与此存在注解差异，不能据此承诺 SetFocus(NULL) 的行为。测试使用实际窗口切换，不依赖 NULL 调用。 |
| [OnSetFocus](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfthreadmgreventsink-onsetfocus) | 第 51/59/63 行：文档视图获得/丢失焦点，当前和此前 manager 可 NULL；SDK `msctf.h:1710,1720` 核对 IID 与声明。 |
| IsThreadFocus / OnKillThreadFocus | 对应官方副本核对“调用线程的 TSF input focus”与 thread UI focus 事件；SDK `msctf.h:1101,10409` 核对声明/IID。GetFocus 非空不等于可写。 |
| [SetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow) | 保存副本第 81/83 行给出非零/零成功失败合同；88–104 行说明前台权限限制，即使满足列出的条件也可能拒绝。没有修改前台策略或使用伪造通知。 |

SDK 为实际读取的 10.0.26100.0 `um/msctf.h`。产品写入路径读到
`platforms/windows/tsf_coordinator/coordinator.cc:448,479,572` 和
`platforms/windows/tsf_edit/edit_session.cc:230`：提交前及真实 edit callback 中仍调用 live guard，未删除这些防线。

## 4. 缺陷、根因与修复

| ID / 级别 | 根因与证据 | 本轮处理与结果 |
| --- | --- | --- |
| TSF-REENTRY-01 / P0 | `OnEndEdit → DropPublished → listener → RequestRebind/Pump` 可在原通知栈内再次开始读取并发布；listener 随后 Shutdown，外层还把 need_bind 设回 true。新增真实 TSF 负例在继承实现上 **exit 5**，出现 binds=2/drops=2、closed=1、awaiting_bind=1。 | 统一 callback/Pump nesting guard；回调适配器保持临时自身引用；先记录待重绑再通知撤销，确保 Shutdown 为终态。负例从 8/10 检查变为最终 9/9，CTest PASS。 |
| TSF-OWNER-01 / P1 | 继承版本仅 Revalidate、write guard 和 OnSetFocus 检查 owner thread，其他 observer 操作/回调没有同等保护。 | 所有状态入口拒绝 foreign thread；Subscribe 必须 live STA；Snapshot 不暴露 foreign-thread 状态；同一 STA 完成释放。新真实 host + foreign-thread/MTA 负例最终 9/9。 |
| TSF-EVIDENCE-01 / P1 | 继承生产决策已要求 live query，但旧 target-evidence 末项仍在 FALSE + lab fallback 时要求把 stale set 标为 notification。基线 CTest FAIL，native exit 5。 | 保留 FALSE 时的 Win32 source 断言，增加 TRUE 时才标 matching notification 的正例。最终 16/16。没有让失效通知取得许可。 |
| TSF-REVOKE-ORDER-01 / P1 | 更换 context 时旧引用释放/新 advice 发生在撤销通知前，且 Drop trace 可能已经失去旧 id/generation。 | 更换前先撤销；document/context 成员先脱离再 Release；回调关闭后不继续更换。相关 focus guard、外部编辑、Push/Pop、资源测试通过。 |
| TSF-HWND-01 / P2 | 继承 helper 使用 thread_local owner，测试仅证明复用，未在断言返回前证明 HWND 销毁。 | 添加显式幂等销毁和 12 次重建/销毁验证，保留 128 次复用测试。resource_balance 最终 51/51。 |
| TSF-DOC-01 / P2 | 旧合同仍包含 skip 77 与“旧通知能覆盖查询失败”的描述。 | 增加明确生效的当前合同，将旧结果标成历史；更新 focus-admission。历史失败未删除。 |

线程销毁采用明确的严格契约：foreign-thread 普通操作拒绝；foreign-thread 析构
`std::terminate`，防止成员析构在错误线程释放 COM。主机须在 owner STA 销毁。
该不可恢复误用路径未运行进程死亡测试；不声称验证任意错误线程销毁均能恢复。
不支持在 active public call/listener callback 中销毁 observer/listener 本体；支持 callback 内 Shutdown。

## 5. 逐文件本轮改动（无 commit SHA）

| 文件 / 行号 | 本轮增量 | 验证 |
| --- | --- | --- |
| `platforms/windows/tsf_context/context_observer.cc:90,197,267,538,964,1088,1279,1312,1391` | nesting guard、owner 检查、先撤销/脱离再释放、callback 自身寿命、STA 订阅及 foreign snapshot 拒绝；失败 GetTop 的非空返回引用也清理。 | reentrant/ownership、18 项 observer 非焦点失败项及关联生命周期通过。 |
| `platforms/windows/tsf_context/context_observer.h:161,233,264,300` | access HRESULT、重入拒绝计数、线程/重入合同。 | 同上，所有依赖重新编译。 |
| `tests/windows/tsf_context_observer/observer_tests.cc:743,868,1013,1051,1276,1387` | 生命周期 trace、解除关联后禁止错 target、新重入/线程负例、HWND 销毁、修正 stale notification 断言并保留正反检查。 | 最终 19 注册/19 执行，18 PASS、real_focus FAIL，零 skip。 |
| `tests/windows/tsf_context_observer/observer_test_support.cc:585` | 创建线程显式释放 helper HWND。 | resource_balance PASS。 |
| `tests/windows/tsf_context_observer/observer_test_support.h:196` | 对应测试 helper 声明。 | 所有相关目标编译通过。 |
| `tests/windows/tsf_context_observer/CMakeLists.txt:108` | 新增 reentrant_shutdown/thread_ownership，沿用真实失败 wrapper。 | inventory 与 JUnit 一致。 |
| `tests/windows/tsf_context_observer/main.cc:29` | 用法列出新增及继承测试项。 | Release 编译通过。 |
| `docs/windows/context-observer-contract.md:3` | 当前合同补充，旧测量保留为历史。 | 对照源码和本轮日志。 |
| `docs/windows/focus-admission-contract.md:27` | live query 优先、published generation 及证据来源合同。 | 对照继承决策和最终 target-policy 22/22。 |
| `docs/reports/go-audit-tsf-20261125.md` | 本报告。 | 原始证据索引核对。 |

没有改生产 coordinator、edit executor、key sink、构建身份或其他 agent 的文件。
未进行无关格式化、第三方原位修改。拒绝的大改方案：没有引入第二套焦点服务/事件总线，
没有以 GetFocus/Pump 代替通知证明，也没有为了 headless 运行改用宽松 admission。

## 6. 实际构建与验证

原始 argv、工作目录、起止 UTC、真实 child exit code 和日志 SHA-256：
`build/go-tsf-20261125/commands.jsonl`；可读完整命令：同目录 `commands.md`。
非零值是实际 CTest/EXE/MSBuild 的值；外层 PowerShell 工具可能统一显示 1，未以其覆盖原值。

| 执行 | 注册/执行 | 通过 | 失败 | skip/error | 原始 exit |
| --- | --- | --- | --- | --- | --- |
| 继承 observer baseline | 17/17 | 15 | 2（real_focus、target_evidence） | 0/0 | 8 |
| 新重入负例 + 继承生产实现 | 独立 EXE | 8/10 检查 | 2 检查 | 无 skip | 5 |
| 第一轮修复定向 | 7/7 | 7 | 0 | 0/0 | 0 |
| **最终 observer** | **19/19** | **18** | **1：real_focus** | **0/0** | **8** |
| **最终关联生命周期/权限/COM** | **32/32** | **32** | **0** | **0/0** | **0** |
| **最终独立 edit 层** | **30/30** | **30** | **0** | **0/0** | **0** |

最后三套合计 81 个不同测试，80 PASS、1 FAIL、0 skip/error；不是全量产品 CTest。
`test-inventory-check.json` 实际比对 CTest JSON 注册清单与每份 JUnit 的名称集合和数量。
32 项包含 17 个 coordinator observed/focus 测试、14 个权限测试、1 个 COM 生命周期测试。
真实异步旧目标、撤权、旧 generation、Unknown/敏感拒绝、迟到 edit/permission、释放配对均在此覆盖。
negative control 的 native exit 5/14 按原设计由 wrapper 核对，不是被跳过的测试。

配置、各有效目标构建 exit 0；一次 `build-fix-r2` 错把独立 `tsf_edit_tests` 指定给 bridge
构建树，MSBuild MSB1009、exit 1，原日志保留。随后读取其独立 CMake 项目，在 `edit/`
重新配置/构建并完整运行 30 项，全部通过。没有把那次错误构建标绿。

最终 observer EXE SHA-256：
`37c63721b77a6144efcc2635c6c2c1a395b601651af149b76a441abd2c72a8d3`。
其余 5 个验证 EXE/DLL 的散列见 `binary-hashes.json`。这些是验证产物，不是安装包或正式 build receipt。

## 7. real_focus：失败和成功证据必须同时保留

继承版曾在显式 `EPOCANVA_TSF_FOCUS_WAIT_FOR_USER=1` 下真实通过 67/67（exit 0）：
`real-focus-inherited-interactive.log`。A/B/空文档/解除关联后再次获焦点均观察到真实
OnSetFocus 在 Pump 前完成；累计 doc_enter/doc_exit=17/17、poll_changes=0。
这是**继承版本**证据，不能替换最终源码的验收结果。

最终完整 observer 套件同样启用这个已有的有界交互等待开关，但实际失败：
`WIN32_FOREGROUND|requested=0|acquired=0|manual_wait_enabled=1`。
窗口已创建，COM 为 MAINSTA、ThreadMgr 已 Activate，两个 thread sink AdviseSink 均 S_OK，
cookie 有效；45 秒内未取得测试 root 的真实前台焦点，因而停止，native exit 5、CTest exit 8。
最终失败点在真正 A/B 切换之前，doc_enter/doc_exit=0/0，不能归因为已证明的订阅泄漏，
也不能当作最终通知测试通过。具体是哪项 Windows 前台输入权限/桌面关系导致拒绝，
本轮没有确定；已证实的近因是窗口未进入前台，**不伪称完整环境根因已排除**。

只枚举过 Windows UI 窗口，未发送键鼠输入、修改系统策略或操作其他应用。
没有强制焦点绕过、sleep 猜时序或手动调用 sink 冒充 OS 事件。
`observer-final.log`、`observer-final-junit.xml` 和派生 focus-trace 均保留失败。

## 8. P0 完成条件逐项判定 / 剩余阻断

| 条件 | 本轮判定 | 证据与完成条件 |
| --- | --- | --- |
| real_focus 真正通过（最终源码） | **未满足：TSF-FOREGROUND-01** | 最终 CTest FAIL。须在可获得真实前台焦点的交互会话，以同一冻结源码重跑；不能排除该测试/设置 skip。当前为运行环境阻断，具体权限机制尚未证实，不是已批准的免验理由。 |
| 独立真实通知证据 | **部分满足** | 继承版 67/67 与 poll_changes=0 是真实证据；最终源码须重新获得通过记录。 |
| 解除关联与焦点变更分开验证 | **合同/关联部分满足** | 官方规范、12 次 association_contract 循环最终 88/88；最终整个获焦点矩阵仍受前台阻断。 |
| 旧目标/过期 generation 不写入 | **受控测试范围满足** | revoked guard、异步 late edit/permission、10 项 focus guard 通过；不外推到未安装的真实宿主。 |
| COM、窗口、线程生命周期 | **已测范围满足** | callback reentry、owner/MTA 拒绝、迟到回调、resource balance、COM lifecycle、edit cleanup 全过；foreign destructor 死亡路径未测。 |
| 产品完整 Release 零失败零 skip | **未满足：TSF-PRODUCT-REGRESSION-01** | 本轮不执行，主链须用冻结源码完整构建/回归；若 real_focus 仍失败必须维持 NO-GO。无需额外审批即可运行获授权本地安全测试。 |
| 获授权真实宿主焦点/安装流程 | **NOT_RUN：TSF-HOST-ACCEPTANCE-01** | 本轮无系统级安装/注册授权；须正式隔离环境和操作者授权，安装后验证真实宿主/线程焦点及长期交互。不能用受控 host 替代。 |

额外限制：此受控未注册 host 的 IsThreadFocus 可以 FALSE，thread-focus set/kill 计数为 0，
即使文档 OnSetFocus 已真实发生。保留 strict fail-closed，没有将其放宽为 Win32 fallback。
产品 trace 开关本轮保持 OFF，测试专用结构化 trace 已记录；未声称取得所有宿主的 thread-focus 通知证明。

## 9. 最短交接路径

1. 主链读取 source-freeze.json，确认本轮 7 个源码/测试文件仍匹配，串行进行根/backend/产品 Release 全量回归。
2. 在同一候选可交互会话中运行完整 observer 和产品 CTest。可使用已有的 45 秒等待开关，
   但必须实际取得焦点；保留所有重跑记录和前次失败，绑定最终测试 EXE 散列。
3. 只有最终 real_focus 及产品全量均通过，才能关闭本地 TSF P0；随后正式安装/宿主验收仍需批准的隔离环境。
4. 当前提交并不干净、没有正式包/CI/审批证据，本报告不能产生产品 GO。

证据入口：`build/go-tsf-20261125/evidence-manifest.json`；原始证据都在其引用的本地文件中，
不是以索引代替源码/JUnit/日志。`audit-result.json` 保持阻断状态。
