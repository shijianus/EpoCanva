# 2026-11-19 内部验收审计报告

## 结论

当前分支的工程回归已通过。根工程、后端、产品 Release 和真实 TSF 焦点路径均有本机零失败、零跳过证据。

发行门禁仍为 **NO-GO**。内部只读 checker 返回 exit 1，`acceptance_ready=false`，报告列出 18 个 blocker。公开发行保持 NO-GO。

## 范围和身份

审计对象是 `feat/internal-acceptance-round`，本地和远端 HEAD 都是

```
c567eb045a72c2b6787edbd3249c7ab7c220e6bc
```

本轮执行了远端 fetch 与 ls-remote，没有 merge、rebase、commit 或 push。产品 verified receipt 记录的当前 source identity 是

```
c567eb045a72c2b6787edbd3249c7ab7c220e6bc+dirty.9430fb7df1eb
source_sha256 = 9430fb7df1ebad1f471538f4436bf44689fde990cd760e83380b32acac4bbfa3
source_files = 562
```

工作区的源码改动只有 `tests/windows/tsf_context_observer/observer_tests.cc`。没有把 dirty fingerprint 当作干净候选 SHA。

## 历史 root JUnit 失败

旧候选 `80cb0bbdd78825036e062416137fb61a06998734` 的 windows-ci #14 / run `43570509740` 保留在旧报告和原始证据中。该次 root 构建和 103 项 CTest 都返回 0，失败发生在 verifier 找不到 JUnit。

根因是 CTest 的相对 `--output-junit build/vs2022-x64-debug/ci/ctest.xml` 会在 `--test-dir build/vs2022-x64-debug` 下形成嵌套路径。当前 workflow 使用绝对路径、清除旧文件并验证文件存在和非空。

本轮根路径回归覆盖旧路径失败、新绝对路径成功和变异负例，8/8 通过。

## 源码修复

`RunRealFocus` 现在在绑定文档 A 前清除桌面级遗留 TSF focus，恢复 A 后先激活真实可见窗口，再调用 `AssociateFocus`。窗口激活使用真实 Win32 foreground、active 和 keyboard focus。关联后有限次重新激活窗口，仍只等待真实 `OnSetFocus`，没有直接调用 sink，也没有把 Pump 重验证当成通知。

关键位置在 [observer_tests.cc](../../tests/windows/tsf_context_observer/observer_tests.cc)

- 清除遗留 TSF focus 约在 574 行。
- 可见窗口激活 helper 约在 620 行。
- 恢复文档 A 约在 639 行。
- 关联后有限重试约在 676 行。

并行只读探针对同一真实 TSF host 的 10/10 次运行显示，程序化 `ITfThreadMgr::SetFocus(B)` 会移动 TSF 内部 focus 但不投递 `OnSetFocus`，`AssociateFocus(window, B)` 加真实窗口焦点会投递通知，清除关联也会投递通知。这与窗口焦点跟踪契约一致。另一次空闲和负载各 12/12 的 real-focus 重复回归也通过，历史失败保留为会话级瞬态证据。

## 实际回归

| 执行 | 结果 | 证据 |
| --- | --- | --- |
| 根工程 Release CTest | 107/107，通过 0、失败 0、跳过 0 | `build/acceptance-audit-20261119/root-release-ctest.log` |
| 根 JUnit verifier | 注册/执行清单严格一致 | `build/acceptance-audit-20261119/root-ci/` |
| 根路径集成回归 | 8/8 通过 | `build/acceptance-audit-20261119/root-path-unit-final-with-ctest.log` |
| 后端 Release CTest | 61/61，通过 0、失败 0、跳过 0 | `build/acceptance-audit-20261119/backend-release-ctest.log` |
| candidate evaluation unit | 77 tests OK | `build/acceptance-audit-20261119/candidate-evaluation-unit.log` |
| backend resource unit | 40 tests OK，2 个明确组件 skip | `build/acceptance-audit-20261119/backend-resource-unit.log` |
| backend release-gate parser | 15 tests OK | `build/acceptance-audit-20261119/backend-release-gate-unit.log` |
| focused real-focus | 1/1 通过 | `build/acceptance-audit-20261119/real-focus-v6.log` |
| real-focus 重复回归 | 12/12 通过，21.21 秒 | `build/acceptance-audit-20261119/real-focus-v7-repeat.log` |
| 前 114 项含 real-focus | 114/114 通过 | `build/acceptance-audit-20261119/real-focus-prefix-v6.log` |
| 正式产品 Release 收据流程 | 257/257，通过 0、失败 0、跳过 0，build/CTest exit 0 | `build/acceptance-audit-20261119/product-build-final-v6.log` |
| 产品 verified receipt | status `verified`，257 total/passed，failed/skipped 0 | `build/acceptance-20261119-product/product-build-receipt.json` |

产品收据绑定的关键二进制摘要如下。

| 文件 | bytes | SHA-256 |
| --- | ---: | --- |
| `EpoCanva.exe` | 506880 | `f2deeb0fcae8c49292dec1e61c5cac4d5241e262af42243a5b0caf757eec8c87` |
| `epocanva_tsf.dll` | 2189824 | `3b487d583bd733aca2748d4669226f9f9ed1e54e05ac81df9149656c5e050ee2` |
| `rime.dll` | 2833920 | `29beb9706230d45b5b26ffe77bb495e35d4b9dc4282bf4a77529ae5cf33002a6` |

产品正式日志显示前一次 real-focus 失败已经消失，最终第 114 项和其后的所有产品测试均通过。

## Stage 与 Corresponding Source

直接传入 backend staging manifest 的 stage 调用以 exit 2 拒绝，原始错误是

```
FAILED: backend payload duplicates a fixed policy destination:
data/backend/en/abbreviations.tsv
```

原因是 `data/manifests/product-package-files.inc` 已固定列出 backend 文件，而 `build/product-resources/product/data/backend/staging-manifest.json` 再次列出同一路径。本轮没有编辑脚本或 policy。使用固定、已 hash 绑定的 product policy 完成了 internal-only stage。

stage 结果为 66 个文件、172136017 bytes，路径为

```
build/acceptance-20261119-product/stage/EpoCanva-0.2.6-w3b
```

stage 日志明确记录该包使用 `internal-unsigned / internal-only` 和 `embedded-build-paths` exception。该 exception 只允许内部诊断包，不能关闭技术门禁。

对应源码 producer 随后成功完成。

| 产物 | 结果 |
| --- | --- |
| 文件数 | 17901 |
| ZIP bytes | 63116693 |
| ZIP SHA-256 | `f9afaa7c8d3a7d0b0c1cb615df8233941f185d6656e821e9b38f4d7a08118cac` |
| manifest | `build/acceptance-20261119-product/source-manifest.json` |

manifest 同时绑定当前 source SHA、package manifest、build receipt 和 package policy。

## Release checker

最新内部检查命令使用 `internal-unsigned`，报告在 [release-readiness.json](../../build/acceptance-20261119-product/release-readiness.json)，原始输出在 `build/acceptance-audit-20261119/release-internal-v7.log`。

checker 通过了 source identity、verified build、engine identity、257 项测试、package policy、66 项 package integrity、package identity、五语言 readiness 和 input stability。它仍然返回 18 个 blocker。

主要 blocker 如下。

- dirty source 不能作为 aggregate acceptance 的 clean revision。
- embedded build-path exception 阻断 technical acceptance。
- internal-only 不授权 public package。
- 当前缺少 hash-bound `release-evidence.json`。
- Corresponding Source 和 candidate quality 尚未绑定完整 release evidence。
- 没有 hash-bound installer artifact 和 installer lifecycle evidence。
- project license、lexicon redistribution、package diagnose、real host、English capabilities、offline privacy、language acceptance、stress reference model、host soak、acceptance CI provenance 均没有当前批准证据。

该 checker 的 exit 1 是预期的门禁结果，不能用测试全绿替代。

## 其他未完成范围

- `ProductLearningWritesPermitted()` 保持 false，产品持久学习写入没有开启。
- Tier B 数据仍为 `not_approved`，绝对质量和性能阈值仍待产品 owner 批准。
- 未执行获授权隔离宿主上的安装、升级、修复、卸载、回滚、多用户和 1800 秒 soak。
- 未产生同一新 clean SHA 的完整六层可信 CI provenance。
- 当前 `EpoCanva.exe` 是产品程序，不是安装 EXE。
- Android IME、完整公开发行范围、公开数据授权和最终签名仍未完成。

## GO/NO-GO

- **本机技术回归：GO**。根 107/107、后端 61/61、产品 257/257，TSF real-focus 正向路径通过。
- **内部验收：NO-GO**。release checker 为 exit 1，`acceptance_ready=false`。
- **公开发行：NO-GO**。没有 public candidate、发行批准和签名链。

本轮没有创建 commit 或 push。下一轮必须先审定源码变更和 staging policy 单一来源，再生成 clean candidate SHA，重建全部 hash-bound 证据并重新运行远端和本地门禁。
