# EpoCanva 决策记录索引

本文件汇总现行记录，不代替原始 ADR、负责人批准或实际验收证据。
本轮审计前此路径不存在；已有决定保留在下列文件中。

| 约束 | 现行记录 |
| --- | --- |
| 英文优先、语言身份与顺序 | [ADR 0001](adr/0001-english-first.md)、[ADR 0005](adr/0005-multilingual-language-profiles.md)、`engine/locale/language_tag.cc` 的 `LanguagePriority` |
| librime 边界与可复现组件 | [ADR 0002](adr/0002-librime-integration-boundary.md)、[ADR 0004](adr/0004-compiled-in-rime-components.md) |
| 离线、隐私与学习语义 | [ADR 0003](adr/0003-runtime-offline-policy.md)、[ADR 0007](adr/0007-local-learning-and-privacy.md)、[ADR 0011](adr/0011-learning-v3-and-recording-read-split.md) |
| 排序与可选模型 | [ADR 0006](adr/0006-adaptive-candidate-ranking.md)、[ADR 0008](adr/0008-optional-neural-prediction.md)、[ADR 0009](adr/0009-shared-base-model-and-domain-fusion.md)、[ADR 0010](adr/0010-model-distribution-and-offline-runtime.md) |
| 项目 AGPL-3.0-only 与第三方分别许可 | [许可决定](product/license-decision.md)、根 `LICENSE`、`third_party/README.md` |
| 内部验收与公开发行门禁 | [产品需求](product/requirements.md)、[发布门禁](product/release-gate.md) |

2026-11-17 本轮用户确认：正式 Tier B 仍 `not_approved`，绝对质量/性能
阈值仍 `pending_product_owner`；没有已确认获授权的隔离 Windows 桌面或 runner。
审批文件保留在 `data/eval/candidate-v1/tier-b.lock.json` 和
`data/eval/candidate-v1/quality-policy-v2.json`，本轮没有授予数据或发行批准。
普通 GitHub `windows-2022`、本机测试和合成 parser fixture 均不能代替正式宿主验收。
总体状态为 `BLOCKED_NOT_COMPLETE`。

产品持久学习写入继续关闭，直到排队、worker、写盘的字段授权和撤销链获得验证；
Unknown 与敏感字段拒绝学习。Windows 普通 TSF DLL 的无网络保证来自代码、依赖
与构建策略审计，不能宣称 Windows 将任意宿主 DLL 的网络权限隔离。
Android 必须省略 `android.permission.INTERNET` 才具有相应平台权限保证；目前尚无
Android IME 实现，不能把该设计要求记为已验证结果。
