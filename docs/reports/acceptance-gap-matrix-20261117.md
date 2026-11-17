# 2026-11-17 需求与实现差距矩阵

审计分支：`feat/internal-acceptance-round`。远端起点和本轮最后确认的 HEAD 为
`80cb0bbdd78825036e062416137fb61a06998734`，不是默认分支。
详细命令、未通过项和证据见 [审计报告](acceptance-audit-20261117.md)。
`implemented` 描述源码存在，不代表正式验收通过。

| 需求/门禁 | 已核对实现 | 本轮状态与差距 | 阻断记录 |
| --- | --- | --- | --- |
| 根工程测试及证据完整性 | workflow、`verify_root_junit.py`、完整注册清单 | #14 的 103 项通过，但证据路径错误使 job 失败；工作区路径修复具有真实 CTest 正反例；新 SHA 的远端执行尚缺 | B01、B05 |
| Windows 组合、UTF-16、异步版本和撤权 | `platforms/windows/tsf_edit`、`tsf_context`、`tsf_coordinator`、产品 key sink | 受控真实 TSF 组件已有实现与测试；新增独立完整文档 oracle；真实焦点通知测试仍有失败，跨宿主未验收 | B02、B03、B07 |
| 完整文档参考模型 | `tests/editor_reference_model` | 五固定种子各 100000 操作；完整文本/选区/组合、受控应用 undo、异步撤销、Unicode 负例、最小化与重放；尚未绑定当前安装包/JUnit 的正式 entry；非折叠选区替换不在当前实现契约内 | B03 |
| 隐私与学习 | `ProductLearningWritesPermitted()`、权限源、存储与后端 | 产品持久写仍 false；Unknown 拒绝；组件撤权与持久化测试不能证明整个产品排队→worker→落盘生命周期；未开启写入 | B04 |
| 候选质量门禁 | `evaluate_candidates.py`、`candidate_quality_policy.py`、`check_release.ps1` | 修复当前审批锁复核、实测阈值复算、非有限数和类型校验；合成测试不构成批准 | B06 |
| 正式 Tier B 与绝对阈值 | `data/eval/candidate-v1` | 真实锁仍 not_approved，绝对阈值 pending_product_owner；现有 15 条 Tier A 不是正式质量结论 | B06 |
| 英文完整能力 | `engine/prediction`、`engine/ranking`、统一 backend 与产品 | 频率/上下文、形态、拼写、分段、缩写已有代码；产品持久写关闭；英文跨应用行为和学习能力仍未正式验收 | B04、B07 |
| 五语言 Windows 输入 | `language-readiness.json` 所列 en/zh/fr/ja/ko 数据和 profile | 五语言后端/受控 TSF 存在；中文 inline、区域/方案、日文部分确认等仍有能力缺口；真实物理输入/安装宿主尚未执行 | B07、B12 |
| 安装生命周期与 1800 秒浸泡 | MSI builder、profile helper、生命周期/soak 消费门禁 | 本轮没有隔离授权环境，安装/升级/修复/卸载/回滚/多用户/宿主浸泡均 NOT_RUN；未对当前桌面执行注册或安装 | B07 |
| 六层可信 CI | 实际 workflow 为 contract/root/canary；input lock/verifier 已存在 | 初始输入 22/22；重建引擎后当前 DLL 与旧 cache pin 不同，最终 21/22、exit 1；四层正式 CI 未启用，普通 hosted runner 不等于正式隔离授权 | B08 |
| Windows EXE 交付 | `EpoCanva.exe` 产品壳、WiX MSI 安装构建器 | 产品 EXE 不等于安装 EXE；安装 EXE 引导器仍缺；MSI 生命周期及最终包也未验收 | B09 |
| AGPL Corresponding Source 与第三方许可 | 完整 LICENSE、许可决定、source contract、数据 provenance 和包策略 | 源归档生产/消费和负例已有；当前产品回归未全绿，不能沿用旧包归档；第三方数据权利与公开分发批准独立 | B10 |
| 内部与公开签名边界 | internal-unsigned/public-signed 分离 | 内部可免签但不能免技术门禁；公开发行批准与最终签名均未提供，公开发行 NO-GO | B10、B11 |
| 干净固定候选 | HEAD 与产品 source fingerprint | 工作区包含修复；未提交、未推送、未冻结新 clean SHA。初始用户改动已保存；dirty 指纹不能冒充 Git 提交 | B05 |
| 来源身份与零跳过收据 | .NET 文件散列、局部失败即停、typed test counts | 已修复读取报错仍返回身份的路径；producer/consumer 均拒绝跳过，具有真实失败注入和摘要已知向量验证 | B13 |
| Windows/Android 完整范围 | 当前 Windows C++ 层与语言身份层 | Android IME 尚无；Spanish/German 与其他世界语言引擎、中文区域/多方案、系统性界面 i18n 尚缺；不能把身份声明当实现 | B12 |

总体仍为 **BLOCKED_NOT_COMPLETE**。最终是否通过只能由固定候选上的真实
`check_release.ps1`、完整必需证据和审批共同决定。

最终本机回归：根工程 **107/107**、后端 **61/61**，均0跳过；产品Release
**256通过/1失败/0跳过**（CTest exit8），失败为真实TSF焦点通知及清关联。
内部checker exit1，21总阻断/18必须技术阻断，acceptance_ready=false；公开checker
exit1、23阻断。源码指纹测试前后保持
`6582fb6955c4a1dd653d4aee6fb3866288c0ab1ed061b84183493eb27bf59360`，
但当前dirty，未产生新clean候选SHA或新提交。
