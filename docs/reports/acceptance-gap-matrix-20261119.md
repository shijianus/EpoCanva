# 2026-11-19 验收差距矩阵

状态：**技术 NO-GO / 内部验收未完成 / 公开发行 NO-GO**。

本矩阵记录当前工作区、远端分支、构建收据、测试和只读 release checker 的真实结果。测试全绿不等同于发行批准。

## 审计身份

- 分支为 `feat/internal-acceptance-round`。
- 本地与远端 `origin/feat/internal-acceptance-round` 都是 `c567eb045a72c2b6787edbd3249c7ab7c220e6bc`。
- 本轮没有 merge、rebase、commit 或 push。
- 当前产品源身份为 `c567eb045a72c2b6787edbd3249c7ab7c220e6bc+dirty.9430fb7df1eb`，source SHA-256 为 `9430fb7df1ebad1f471538f4436bf44689fde990cd760e83380b32acac4bbfa3`，包含 562 个 source files。
- 未提交的源码变更只有 `tests/windows/tsf_context_observer/observer_tests.cc`。工作区另有并行会话产生的未跟踪报告，未纳入产品源指纹。

## 差距矩阵

| 需求或门禁 | 当前结果 | 状态 | 阻断 |
| --- | --- | --- | --- |
| 历史 root JUnit 失败处置 | 旧 SHA `80cb0bbdd78825036e062416137fb61a06998734` 的相对 JUnit 路径失败原始证据保留。当前路径回归 8/8 通过。 | 已修复 | B01 已关闭 |
| 根工程完整测试 | Release CTest 107/107，通过 0、失败 0、跳过 0。根 JUnit verifier 严格清单一致。 | 通过 | — |
| 后端完整测试 | Release CTest 61/61，通过 0、失败 0、跳过 0。 | 通过 | — |
| 产品完整测试 | 正式产品收据流程 257/257，通过 0、失败 0、跳过 0，build 和 CTest 均为 0。 | 通过 | — |
| TSF 真实焦点 | 真实窗口 `OnSetFocus`、B 文档绑定、清除关联路径均通过。聚焦测试和前 114 项序列通过。 | 已修复并复验 | — |
| 产品 verified receipt | 收据 status 为 `verified`，66 项之外的测试数量严格为 257，三项二进制 hash 已绑定。 | 通过 | — |
| 内部产品 stage | 固定 policy stage 成功，66 files、172136017 bytes。传入 backend staging manifest 的原始调用被重复目的地检查拒绝。 | 已完成，保留冲突证据 | B02 |
| Corresponding Source | ZIP 17901 files、63116693 bytes，SHA-256 为 `f9afaa7c8d3a7d0b0c1cb615df8233941f185d6656e821e9b38f4d7a08118cac`。 | 产物已生成 | 仍受 release evidence 绑定阻断 |
| 内部 release checker | `acceptance_ready=false`，18 个 blocker，exit 1。 | 未通过 | B03–B10 |
| 持久学习 | `ProductLearningWritesPermitted()` 仍返回 false。 | 有意关闭 | B05 |
| Tier B 与绝对阈值 | Tier B 仍 `not_approved`，绝对质量/性能阈值仍待 owner 批准。 | 未完成 | B06 |
| 安装、真实宿主、浸泡 | 未执行获授权隔离宿主、安装生命周期、跨应用输入和 1800 秒 soak。 | 未运行 | B07 |
| 六层可信 CI | 当前证据不是同一新 clean SHA 的六层真实 CI provenance。 | 未完成 | B08 |
| EXE 安装器、公开签名 | 没有 hash-bound 安装器和 public-signed candidate。 | 未完成 | B09、B10 |

## 当前阻断

| 编号 | 阻断 | 证据与下一步 |
| --- | --- | --- |
| B01 | 干净候选 SHA | 源码变更尚未提交。需在审定后提交、冻结 SHA、重建并跑远端 CI。 |
| B02 | staging 输入重复 | `product-package-files.inc` 已固定列出 backend 文件，`staging-manifest.json` 又列出相同路径，原始调用以 duplicate destination 退出 2。当前使用固定 policy stage，未修改脚本或 policy。应统一两套 payload 来源。 |
| B03 | acceptance profile | 当前内部包使用 `embedded-build-paths` exception，checker 明确阻断技术 acceptance。需生成无嵌入路径的严格包。 |
| B04 | release evidence | `release-evidence.json` 缺失，checker 不推断审批、runner 或测试授权。 |
| B05 | 持久学习 | 产品写入保持关闭。需完成排队、worker、落盘、撤权和清除的授权生命周期证明后再评估开启。 |
| B06 | 质量批准 | 需要批准的 Tier B 数据、来源、许可、Top-1/Top-3/MRR 与绝对性能阈值尚未提供。 |
| B07 | 真实宿主 | 需要隔离授权、安装/升级/修复/卸载/回滚、多用户、五语言输入、隐私和 1800 秒 soak。 |
| B08 | CI provenance | 需要同一 clean SHA 的 backend、windows-product、integration、quality/stress 等完整层级和 runner 授权。 |
| B09 | 安装器 | `EpoCanva.exe` 不是安装 EXE。需生成 hash-bound 安装器并执行生命周期。 |
| B10 | 公开发行 | public candidate、数据再分发批准、对应源码交付批准、最终签名和时间戳均缺失。 |

## 判定

- **当前技术回归：GO**。根、后端、产品和 TSF 测试已通过。
- **内部验收：NO-GO**。只读 checker 仍有 18 个 blocker，`acceptance_ready=false`。
- **公开发行：NO-GO**。内部 unsigned/internal-only 包不构成公开发行授权。

## 后续顺序

1. 审查并合并 TSF 测试改动，形成 clean candidate SHA。
2. 修正固定 policy 与 backend staging manifest 的单一来源关系。
3. 重新生成无 embedded build-path exception 的产品包和 source manifest。
4. 绑定真实 release evidence、质量批准、宿主授权和六层 CI provenance。
5. 对同一 SHA 重建根、后端、产品、质量、隐私、压力和安装生命周期证据。
6. 完成公开数据授权、对应源码交付和签名后再做 public GO/NO-GO。

详细命令和原始日志见 [2026-11-19 审计报告](acceptance-audit-20261119.md)。
