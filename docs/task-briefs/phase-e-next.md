# 阶段 E 任务书 — 默认智能排序与多语言候选合并

## 0. 给执行者

本文件可直接交给下一轮 CLI Agent。它假设你**没有**本轮对话上下文；所有必要事实都
写在下面。不要依赖记忆或上一轮总结。

仓库：`https://github.com/shijianus/EpoCanva`
起点分支：`feat/offline-multilingual-backend`
起点 HEAD 以实际 `git rev-parse HEAD` 为准；不要假设仍是 `7442820`。
开工前执行：

```bash
git rev-parse HEAD
git branch --show-current
git status --short
git log --oneline -10
```

若工作区有未提交改动，先识别来源并保护，不要覆盖或回滚。

## 1. 阶段 D 已交付的事实（需自行复核）

- `engine/data_store/learning_store.{h,cc}`：schema v3，profile/language、候选 ID、
  正负摘要、UTC 毫秒衰减、手动词条、v1/v2 迁移、原子拒绝坏格式；上限 2048 word /
  2048 pair / 448 preference / 262144 bytes。
- `engine/data_store/windows_learning_repository.{h,cc}`：原子写、`.v2` 备份、
  `.corrupt` 隔离、`all.lock`、`DeleteAll`；读取/暂停语义。
- `engine/ranking`：`SetPreference` 持久化安装器；后端 `ApplyPersonalRanking` 只对
  COMPLETION 候选做有界只读重排，原文首选，NEXT_WORD 未接。
- `backend.learning.persistence_ranking` 与 `backend.profiles.real_acceptance` 通过。
- 产品 `ProductLearningWritesPermitted()` 恒 false；TSF 字段锁未证明，产品不写学习。
- 根 78 条、后端 58 条、产品定向 29 条测试通过；产品全量曾有 1 个
  `sink_settings_timing` 时序抖动，单独重跑通过。

阶段 D **没有**做：多语言候选合并、profile 清单合并、排序校准、NEXT_WORD 个性化、
Android、模型下载、28 语言完整实现、真实 TSF 宿主验收。

## 2. 阶段 E 唯一主目标

**把默认（无神经模型）候选排序做成可解释、可校准、可回归的多语言候选合并路径，
并保持原文候选硬保护和既有编辑/候选链路不退化。**

阶段 E 不打开产品持久写入；字段锁问题属于独立门槛，不由本阶段绕过。

## 3. 必须完成的工作

### E1 单一 profile 清单

- 合并 `engine/locale` 的 profile 注册表与 `tools/backend/stage_resources.py` 的 13 个
  profile；只保留一个机器可读来源，或让 staging 从注册表生成。
- 每个 profile 必须有：完整 BCP 47 身份、短 primary language、输入方案、输出模式、
  键盘布局、资源/词典身份、实现状态（已实现/未验证/缺失）与本地证据路径。
- 不把完整 BCP 47 塞进只接受短主语言 ID 的字段；不注册没有资源的语言。
- 完成定义：两处清单由同一数据源产生；有负向测试证明重复/冲突/缺失证据会被拒绝；
  staging 测试仍通过。

### E2 统一候选身份、来源与语言标签

- `Candidate`/`Proposal` 增加或统一：稳定 candidate ID、来源（exact/completion/
  spelling/morphology/abbreviation/conversion/prediction/original）、语言标签、
  原始输入范围、可学习标志。
- 候选 ID 不得使用候选菜单 token；使用 admitted lexeme ID 或明确的手动词条 ID。
- 非法 UTF-8、控制字符、超长/重复 ID 拒绝，不截断。
- 完成定义：跨语言候选去重、同一 candidate ID 在重复查询中稳定；单测覆盖非法
  UTF-8/控制字符/边界 ID/重复 ID。

### E3 接入真实候选源

- 逐个接入 en/zh/fr/ja/ko 的真实候选源（现有的后端/会话/词典），不使用固定候选数组。
- 明确每个源的语言、输出模式、候选预算、是否可学习。
- 完成定义：每个源至少一条真实输入→真实候选的集成测试；候选来源和语言在候选
  元数据中可见；源失败时回退而非伪造。

### E4 跨语言规范化与去重

- 统一大小写、空白、Unicode 规范化（若采用，必须声明版本与范围）、简繁/地区
  差异的处理策略；不得用简繁转换冒充地区变体支持。
- 同一显示文本跨源合并时保留最佳来源/分数，但不得把未标定原始分直接相加。
- 完成定义：en+zh、en+fr、en+es、en+ja、en+ko 的受控混合输入有稳定顺序；
  重复候选只出现一次；原文候选首位。

### E5 显式语言优先与检测锁存

- 用户显式选择的语言/方案始终优先；自动检测只作有界辅助。
- 检测必须锁存（连续 N 次一致观察才切换），避免英法/英西短文本抖动。
- 完成定义：单次/交替观察不改变顺序；连续一致观察才切换；显式语言在分数接近时
  仍优先；有确定性重复查询测试。

### E6 排序特征校准

- 为以下特征声明单位、区间和钳制：前缀质量、词形/正规化匹配、可靠词频/N-gram、
  用户正/负偏好、时间衰减、领域相关性、来源惩罚、语言匹配、原文硬分层。
- 不同来源的原始分数不得未经标定直接相加；权重必须可被离线评测替换。
- 使用已有真实语料/模型做离线评测，至少报告：Top-1/Top-3 下一词、MRR、CJK 下一字/
  词命中、混合语言准确率；与无模型基线比较。
- 完成定义：有可复现的评测脚本/数据清单；权重变化有前后对比；没有评测就不得称
  “已校准”；保留无模型回退路径。

### E7 混合语言回归

至少覆盖：

- English+Chinese（含拼音/简拼、英文前缀混输）；
- English+French（重音、大小写、词形）；
- English+Spanish（重音/地区变体，若仍无引擎则诚实标缺失）；
- English+Japanese（Romaji/Kana/Kanji）；
- English+Korean（两套式/原生 Hangul）；
- Chinese+English+专业术语（受控词表）。

完成定义：每条至少一个真实候选源集成测试、一个重复查询确定性断言、一个原文保护
断言；无引擎语言标 NOT_IMPLEMENTED，不伪造。

### E8 不依赖神经模型的下一词预测

- 在现有 n-gram/上下文模型上改善下一词候选，至少 Top-1/Top-3 指标和 p50 延迟；
- 下一词候选必须仍是候选，不自动提交；模型/上下文缺失时回退。
- 完成定义：有基线对比与回退测试；延迟/内存有实测数字；未达门槛不接入产品。

## 4. 不允许越过的边界

- 不执行 git commit/push/tag/PR；不注册 TSF、不提权、不安装、不改系统配置。
- 不下载任何模型权重；不修改 `third_party/`；不新增网络 I/O。
- 不打开产品持久学习写入；不把测试 double 冒充字段锁。
- 不把未校准分数相加后称为智能排序；不删除/放宽失败测试。
- 不把简繁转换当地区支持；不注册没有引擎/资源的语言。
- 不重写已验证的 TSF 编辑/选择/候选链路。

## 5. 建议顺序

1. E1 清单合并（先红测试：重复/冲突/缺失证据）。
2. E2 候选身份与来源字段（先改核心类型，再改后端适配）。
3. E3 逐源接入（每源一个真实集成测试）。
4. E4/E5 合并、去重、语言锁存（混合语言回归）。
5. E6 离线评测与权重替换；没有评测不调默认权重。
6. E8 下一词预测；最后才考虑产品候选接线。

## 6. 验收证据

- `docs/reports/` 新报告：实际命令、退出码、通过/失败/跳过数量、负向控制。
- 每项能力列 SOURCE_VERIFIED / UNIT_TESTED / INTEGRATION_TESTED / HOST_VERIFIED /
  BLOCKED / NOT_IMPLEMENTED；不得把单元测试说成宿主验收。
- 文件清单完整；负向控制至少一条真实转红再恢复。
- 现有根 78、后端 58、产品定向 29 不得退化；新增测试必须能失败。

## 7. 已知阻塞（不要假装解决）

- 产品持久写入：字段锁未覆盖 worker 全链路，保持关闭；阶段 E 不解决。
- 当前包真实 TSF 宿主验收：缺 UAC/会话授权，标 BLOCKED。
- 公开数据再分发：42k 词表、中日词典、语料授权未完成。
- 无 CI；所有结论来自单机执行。
- 产品全量 `sink_settings_timing` 有低负载通过、高负载偶发失败的时序风险；阶段 E
  若触碰设置/部署路径，必须重新验证并记录。

## 8. 一句话完成定义

**在无神经模型的默认路径上，多语言候选来源可统一、可去重、可解释地排序；原文候选
仍首位；英中/英法/英西/英日/英韩混合输入有真实集成回归；排序权重经过离线评测而非
凭感觉调整；既有 78/58/29 测试不退化。**
