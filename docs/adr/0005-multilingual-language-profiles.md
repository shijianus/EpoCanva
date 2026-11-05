# ADR 0005 — 多语言身份以 BCP 47 profile 表达，优先级只有一份定义

## 状态

Accepted。实现：`engine/locale/language_tag.{h,cc}`、`engine/locale/locale_profile.{h,cc}`；
测试：`tests/locale_registry`（3 条 CTest）。

## 背景

审计发现三处互相独立、但会一起失效的事实：

1. 代码只认 5 个硬编码语言 id（`engine/language_packages/language_package.cc:16-18`），
   ABI 也只映射 1–5（`engine/backend/backend_api.cc:40-49`）。
2. `variant` 字段只做字符串比较（`backend_api.cc:451-462,472`），
   `zh-Hans-CN` / `zh-Hant-TW` 被直接拒绝，且不改变词库或排序。
3. 语言优先级在 `language_packages` 里第二份硬编码，与产品规格分开维护。

新产品规格要求 7 个核心语言、28 个声明语言、中文四个独立变体与六种输入方案。
继续用"枚举 + 字符串比较"会让每加一个语言都要改三处代码。

## 决策

1. **语言身份统一用 `LanguageTag`（BCP 47 子集）表达**：language + 可选 script +
   可选 region + variants。扩展与私有子标签**明确拒绝**，不是接受后忽略。
2. **优先级只有一个定义**：`locale::LanguagePriority`。`language_packages::LanguagePriority`
   委托它，删除第二份列表。
3. **一个语言标签对应一个 profile**，profile 内部有多条输入方案。
   profile 的持久化键是 `profile_id`（ASCII），显示名不参与查找、排序或持久化。
4. **输入方案三级实现状态**：`kImplemented` / `kUnverified` / `kAbsent`。
   `kImplemented` 必须带本地证据引用；没有证据的"已实现"注册被拒绝。
   `Resolve` 只在 `kImplemented` 时返回 `kResolved`。
5. **歧义不猜**：只给 `zh` 时匹配到 Hans/Hant 多个 profile，返回 `kAmbiguous`，
   由调用方补充 script/region。
6. **没有引擎就没有 profile**：Spanish / German 在代码里有身份与优先级，
   但目录中没有条目，解析返回 `kNoProfile`。

## 备选方案与未采纳原因

| 方案 | 未采纳原因 |
| --- | --- |
| 继续用 `enum { EN=1, ZH=2, ... }` 扩展 | 无法表达 script/region/variant；每加语言都要改 ABI、校验、目录三处 |
| 用 ICU 的 Locale | 体积、许可与跨平台评估成本远超本轮需要；本层只需要身份与匹配，不需要排序规则或格式化 |
| 直接存显示名作为键 | 显示名会随翻译变化，破坏持久化稳定性与隐私（可推断族群） |
| 只给 `zh` 时默认 Simplified | 静默猜测会误导繁体用户；歧义必须显式解决 |
| 把 Spanish/German 注册成只有名字的 profile | 违反"不得为凑数量只注册名称"；会让设置界面出现不可用选项 |

## 后果

**正面**

- 中文四个变体、六种方案、28 个语言都有明确的身份位置，不需要改 ABI 枚举。
- "已实现"必须有证据，不能靠声明；`kUnverified` 与 `kAbsent` 让诚实状态可表达。
- Android 复用不再依赖 Windows 侧的语言常量。

**负面 / 成本**

- 多了一层解析与匹配；调用方必须处理 `kAmbiguous`。
- `language_packages` 现在依赖 `locale`，独立配置时必须一起引入。
- 代码内目录与 `tools/backend/stage_resources.py` 生成的 13 个 profile
  **尚未合并**，两套清单可能分叉。这是登记在案的已知风险。

## 重新评估条件

1. 需要支持扩展/私有子标签（例如第三方方案命名空间）时。
2. 需要 Unicode 完整 Locale 语义（排序、日期、数字格式）时——那时才评估 ICU。
3. 代码内目录与 staging 清单合并为单一数据源后，本 ADR 的 §决策 3 需要改成"从数据文件加载"。
