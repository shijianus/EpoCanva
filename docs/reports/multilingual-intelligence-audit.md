# EpoCanva 全面独立复审与多语言智能输入架构审计

状态：**审计完成，工程切片已落地并通过测试；完整产品验收未通过。**

本报告是本轮独立审计的实际结论，不引用此前阶段报告的完成声明。所有代码事实都给出
本地文件与行号；所有测试结论都给出实际执行的命令与退出码。证据等级严格区分，不合并为
一个模糊的"完成"。

## 1. 仓库状态

| 项目 | 实际值 |
| --- | --- |
| 工作区根 | `D:/Users/Smart Kevin/OneDrive/Desktop/EpoCanva` |
| HEAD | `84788168f89fe8467492d4b614828e530aa91845` |
| 分支 | `feat/offline-multilingual-backend` |
| 工作区起始状态 | 干净（`git status --short` 无输出） |
| 远端 | `origin https://github.com/shijianus/EpoCanva.git` |
| `refs/remotes/origin/main` | `5c0f11b1fa94c965e1a00f920272860c2d20ff73` |
| 审计时间 | 2026-11-05 |
| 环境 | Windows 10.0.26200 x64，Git Bash，VS2022 Professional / MSVC 19.44.35209.0，CMake 3.31.6-msvc6（VS 随附），Python 3.14 |
| 本轮未执行 | 任何 `git commit` / `push` / `tag` / PR；任何 UAC 提权、系统注册、安装或卸载 |

补充事实（只读探测）：

- `reg query` 对 `HKLM\SOFTWARE\Classes\CLSID\{B440F702-63E4-4D47-BCC4-074899ABA836}` 与
  `HKCU\...` 同路径均返回"unable to find the specified registry key or value"。
  即**当前机器上产品 TIP 未注册**。
- `.github/` 不存在：**没有 CI 工作流**。
- `third_party/patches/` 不存在；`third_party/librime/` 是固定提交的独立工作副本，
  由 `.gitignore` 排除。AGENTS.md §7 描述的补丁流程目前**没有被任何补丁使用**。
- `platforms/` 下只有 `windows/`：**没有 Android 实现**。

## 2. 证据等级定义

| 等级 | 含义 |
| --- | --- |
| SOURCE_VERIFIED | 在本地源码中找到真实实现，并核对了调用链 |
| UNIT_TESTED | 单元测试在本机真实编译并通过 |
| INTEGRATION_TESTED | 多模块集成链路在本机真实通过 |
| HOST_VERIFIED | 在真实系统宿主（记事本/浏览器/Office）中验收通过 |
| RELEASE_READY | 公开发布条件全部满足 |
| BLOCKED | 缺工具、许可或授权 |
| NOT_IMPLEMENTED | 尚未实现 |

## 3. 逐项审计结论

### 3.1 英文能力实现了多少，哪些只在夹具中有效

**SOURCE_VERIFIED + UNIT_TESTED。整体判定：部分实现，不满足 AGENTS.md §1 的完整英文要求。**

真实存在且有独立单测的英文能力（`engine/languages/en/`，不包含任何 Rime 或平台头文件）：

| 能力 | 实现位置 | 证据 |
| --- | --- | --- |
| 词表加载、前缀查询 | `english_lexicon.{h,cc}`、`prefix_completer.{h,cc}` | 单测 + 引擎内组件 |
| ASCII 大小写映射 | `english_case.{h,cc}` | `tests/english_core` |
| 词形/受保护文本判定 | `english_token.{h,cc}` | `tests/english_core` |
| 空格与提交边界 | `english_space_policy.{h,cc}` | 纯函数单测 + `english_space_processor.cc` |
| 形态变化 | `morphology_rules.{h,cc}` | `language_support` 内被调用 |
| 一次编辑拼写建议 | `spelling_suggester.{h,cc}` | 需显式开启 `english/enable_suggestions` |
| 连写切分/分段 | `language_support.cc` 的 segmentation 分支 | 有实现，但只在受控词表上验证 |
| 频率/上下文排序 | `language_support.cc:24-26,264-267` | 单测 |

**仍未实现或未验收的部分**（因此 §1 不成立）：

- 只有 ASCII case mapping；没有 Unicode case folding、没有句首自动大写、
  没有专名/品牌词大小写恢复（`iPhone`、`GitHub` 仍不补全）。
- 词形只有"至多一个 ASCII 分隔符"的有限规则；没有 Unicode 弯撇号/破折号规范化。
- 没有 URL/邮箱/代码解析器，只有"遇到停止字节就放弃补全"的保守策略。
- 跨应用一致性只有产品层共享状态的设计，**没有真实宿主矩阵验收**。
- 生产词频不存在：Alt12Dicts 权重是"被几部源词典收录"的分层，**不是语料频率**。
  这一点在当前代码里没有造成错误结论，但任何"频率排序"的对外表述都必须加这句限定。

### 3.2 正式产品词库与 pilot 验证词库是否被正确区分

**SOURCE_VERIFIED。区分正确。**

- 产品词表由 `tools/data/build_product_lexicon.py` 从固定 ESDB 提交确定性生成，
  记录 provenance，进入内部 Alpha 暂存包。
- pilot 清单由 `tools/data/build_english_pilot.py` 生成到 `build/english-pilot/`，
  **不进版本库、不打包**（`docs/compliance/english-pilot-data.md`）。
- 两者共用同一转换逻辑但产物身份分开；`tests/english_pilot/assert_pilot_output.cmake`
  逐项对齐锁定清单。
- 默认测试夹具是 54 行手写词表，与 42,356 行清单不是同一份数据。

### 3.3 P2.6 拼写建议的真实启用边界

**SOURCE_VERIFIED。**

- schema 开关是 `english/enable_suggestions`，只有
  `tests/fixtures/english_spelling/epocanva_spell_english.schema.yaml` 打开它。
- 产品默认关闭；`ProductSettings` 中拼写建议、形态建议、二次编辑拼写都是显式 opt-in，
  只有缩写展开在 2026-11-05 复审后改为默认开启。
- 拼写建议**不自动替换**宿主文本，必须用户显式选择候选。
- 代价模型在 `lexical_service.h:44-55`：`−4*OSA_edits −0.005*keyboard_cost`；
  键位模型区分 US QWERTY / French AZERTY / German QWERTZ / Dvorak，未知布局用中性成本。

### 3.4 Windows TSF 的生命周期、候选、范围校验和原文提交路径

**SOURCE_VERIFIED。** 关键结论：

- 普通按键**不写宿主文本**：`text_service.cc:626-645` → `text_service_controller.cc:490-532`
  → 统一路径返回 `false`（`unified_host_input.cc:5-19`）。只有显式选择候选才写。
- 选择路径的校验顺序（任一失败都直接返回、不写文本）：
  1. `FinalizeCandidateSelection`：快照版本/上下文/代际/光标相等
     （`text_service_controller.cc:1129-1132`），无在途工作（`:1153-1156`），
     菜单已打开（`:1170`），期望光标（`:1177-1184`），实时光标（`:1191-1221`）。
  2. `SubmitReplacement` → 协调器阶段/边界/退役/准入，目标重校验
     （`coordinator.cc:1778-1798`），候选快照/身份/光标校验（`:1800-1815`）。
  3. 统一后端选择 + 提交（`:1817-1828`），写入前查询字段权限
     （`backend_integration.cc:134-144`）。
  4. 编辑会话预检：关闭/绑定/上下文 ID/代际/退役/不确定/忙/权限
     （`edit_session.cc:540-601`），目标复检（`:611-635`），只读检查（`:637-659`）。
  5. `RequestEditSession` 回调内再次执行陈旧检查、目标复检、权限复检、只读检查，
     之后才 `writer.Apply`（`:283-284`）。
  6. `ReplaceToken` 拒绝活动组合/空文本/折叠选区/光标不等/范围不符/文本不符/
     ASCII 边界不符，之后才 `SetText`（`composition_writer.cc:651-776`）。

判定：**SOURCE_VERIFIED + 产品内受控宿主 INTEGRATION_TESTED；当前包的 HOST_VERIFIED 不成立**。

### 3.5 Windows C 层真实注册、输入与卸载是否有新证据

**SOURCE_VERIFIED（实现）+ 历史 HOST 证据（旧包）+ 当前包 BLOCKED。**

- 导出恰好四个：`DllGetClassObject` / `DllCanUnloadNow` / `DllRegisterServer` /
  `DllUnregisterServer`（`platforms/windows/tsf/epocanva_tsf.def:19-23`）。
- 注册策略：只写本产品 CLSID / `InprocServer32` / 两个实现类别，支持 HKLM 与 HKCU，
  逐项回读，失败回滚，拒绝外来 CLSID（`registration.cc:711-775`）。
- **没有任何 CMake/CTest 调用注册**；测试只断言导出存在并显式声明不调用
  （`tests/windows/tsf_lifecycle/main.cc:155-166`）。注册测试用 `FakeRegistry`。
- 本机当前未注册（§1 的 `reg query` 结果）。
- 2026-11-02 的六轮真实会话属于**旧包**；当前源码对应的包没有新的宿主会话。
  候选窗口在真实宿主中的可见性仍未证明。

判定：**注册与卸载代码 SOURCE_VERIFIED；当前包 HOST_VERIFIED = BLOCKED（缺授权与新会话）**。

### 3.6 是否已有自有稳定 C ABI

**SOURCE_VERIFIED，不再只是"架构文件里写了"。**

- `engine/include/epocanva_engine.h`：`EPOCANVA_ENGINE_ABI_VERSION 1u`（:24），
  13 个导出函数（:235-298），`struct_size_bytes` + `abi_version` 版本协商，
  调用方缓冲区、无跨边界异常/分配器，所有文本偏移显式命名 `*_utf8_byte_offset/count`。
  实现于 `engine/core/engine_api.cc:191-474`。
- `engine/include/epocanva_backend.h`：v1 后端 14 个函数（:172-216），
  追加式 v2 profile 接口 6 个（:297-320），`EPOCANVA_BACKEND_PROFILE_ABI_VERSION 2u`（:219）。
  实现于 `engine/backend/backend_api.cc` 与 `profile_api.inc`。
- 错误码分段（`epocanva_engine.h:43-64`、`epocanva_backend.h:46-51,241`），
  `epocanva_status_name` 返回静态 ASCII。
- 句柄是 `uint64_t` 不透明令牌，不是指针也不是 Rime ID。

判定：**ABI 已存在且被单测与 C 消费者覆盖**（`tests/engine_abi/c_consumer.c`、
`tests/backend/c_consumer.c`）。但 v2 profile 结构仍是**调用方传入**，没有数据驱动注册表；
本轮新增的 locale 注册表填补了身份层，尚未替换 ABI 参数。

### 3.7 用户词库与学习功能是否真正存在

**SOURCE_VERIFIED + UNIT_TESTED（存储层）；产品路径 NOT_ENABLED。**

- `engine/data_store/learning_store.{h,cc}`：ID/计数模型，上限 2048 词 + 2048 上下文对、
  256 KiB、schema v2（v1 可读并迁移），隐私分类在每次操作与每次写入前检查。
- `engine/data_store/windows_learning_repository.cc`：`learning-v2/n-<hex>.dat`，
  临时文件 + `FlushFileBuffers` + 句柄重命名实现原子写，孤儿 `.tmp` 清理。
- **没有时间衰减、没有负反馈、没有时间戳字段**（`learning_store.h` 的
  `Snapshot`/`WordCount`/`PairCount` 只有 `revision` 与计数）。
- 产品路径：`platforms/windows/engine_executor/unified_engine_backend.cc:198`
  固定 `request.learning_enabled = 0`，`:165` 从不提供 `learning_root`。
  即**产品学习持久化未启用**。

判定：存储层 UNIT_TESTED；产品学习 NOT_ENABLED。

### 3.8 是否有多语言 Scheme/Schema、Locale Registry、语言切换和对应测试

**审计前：部分。审计后：身份层已补齐，引擎层仍缺。**

审计前的事实：

- 代码只认 5 个语言（`engine/language_packages/language_package.cc:16-18`），
  ABI 映射 1–5（`engine/backend/backend_api.cc:40-49`）。
- 13 个 profile 由 `tools/backend/stage_resources.py` 暂存，覆盖
  en（多键盘布局）、zh（拼音/注音）、ja（Romaji/Kana/Hiragana/Katakana/Kanji）、
  ko（两套式/原生 Hangul）。
- `variant` 字段只做字符串比较（`backend_api.cc:451-462,472`），
  **不改变词库或排序**；`zh-Hans-CN`、`zh-Hant-TW` 在审计前被拒绝（`:462`）。
- 没有 BCP 47 解析、没有 script/region 变体匹配、没有简繁/粤语身份。
- **没有跨语言候选合并**：每个会话只有一个语言（`backend_api.cc:146,503`），
  `Candidate` 没有语言字段（`backend_session/session.h:26-35`）。

本轮新增（见 §5）：`engine/locale` 提供 BCP 47 解析、lookup 匹配、
七语言固定优先级、8 个内置 profile 与诚实的能力分级。

### 3.9 Android 是否存在真实实现

**NOT_IMPLEMENTED。** `platforms/` 只有 `windows/`；没有 `android/`、
没有 `InputMethodService`、没有 `build.gradle`、没有 APK 产物。
因此"不申请 INTERNET 权限"的平台级离线保证目前**只是一条产品要求**，不是已生效的事实。

### 3.10 是否已有可选模型框架、推理运行时或模型资源管理代码

**部分：推理代码存在，产品接线与资源管理不存在。**

- `engine/neural/lexical_reranker.{h,cc}` 是**真实推理**：Qwen2 GGUF 维度/词表校验
  （`:276-331`）、tokenize、`llama_decode`、log-softmax 求和、稳定排序（`:355-468`），
  模型文件用 SHA-256 校验（`:289-291`），`llama_model_load_from_file`（`:308`）。
- **没有接进产品**：`engine/backend/CMakeLists.txt` 不链接 neural 目标；
  引用只在 `engine/neural/` 与 `tests/neural_reranker/`。
- **默认不启用**：`data/manifests/backend-readiness.json:17`
  `neural_default_admitted=false`。
- **没有模型目录、没有下载器、没有 SHA 清单驱动的安装状态**。
  `data/manifests/backend-model.sources.json` 是研究记录，不是模型管理实现。

判定：推理 SOURCE_VERIFIED；产品接线、资源管理与分发 NOT_IMPLEMENTED。

### 3.11 哪些模块仍在依赖 Windows 专用类型，妨碍 Android 复用

`grep` 实际结果（`engine/**` 内含 Windows 头文件的文件）：

| 文件 | 依赖 | 影响 |
| --- | --- | --- |
| `engine/backend/context_text.h:3` | `<windows.h>`，`GetStringTypeW` | 上下文分类，Android 不可用 |
| `engine/backend/local_resource.h:3` | `<windows.h>`，`CreateFileW` | 资源加载 |
| `engine/data_store/windows_learning_repository.cc:9` | `<windows.h>` + 非 Windows `#error` | 持久化适配器 |
| `engine/french_dictionary/dictionary.cc:17-18` | `<windows.h>`、`<bcrypt.h>` | 法文字典哈希/路径 |
| `engine/lexical_service/text.cc:18` | `<windows.h>`（有非 Windows 回退：`:88,113,216,274,286`） | Unicode 规范化 |
| `engine/neural/lexical_reranker.cc:10-11` | `<windows.h>`、`<bcrypt.h>` + 非 Windows `#error` | 神经重排 |

构建级阻断更严重：`engine/backend/CMakeLists.txt:3`（非 WIN32/MSVC 直接 fatal）、
`:53`（编译 `platforms/windows/product/product_io.cc`）、
`engine/core/CMakeLists.txt:5-6`、`engine/neural/CMakeLists.txt:4`、
`engine/french_dictionary/CMakeLists.txt:15`、
`engine/japanese_decoder/CMakeLists.txt:3`（同样编译 `product_io.cc`）。

结论：**语言算法层（en/zh/ja/ko/locale/ranking）是跨平台的；后端装配层不是。**

### 3.12 哪些文档的状态与实际代码不一致

**以下不一致已在本轮修订或登记：**

| 文档 | 不一致 | 处理 |
| --- | --- | --- |
| `docs/architecture/overview.md` | §5 声称 `epocanva_engine.h`、engine-core、data-store、rime-adapter、Session 全部"未实现"，而 ABI 与 learning store 已存在 | 本轮重写 §5 与相关段落 |
| `docs/architecture/overview.md` §2.5 | 称 `english-morphology`/`english-corrector`/`english-segmenter` 未实现，实际 `morphology_rules.cc`、`spelling_suggester.cc`、segmentation 分支已存在 | 本轮更正 |
| `AGENTS.md` §1 | 语言优先级仍是 5 语言，与新产品目标冲突 | 本轮修订为 7 语言并保留其余永久规则 |
| `docs/product/requirements.md` §3 | 同上 | 本轮修订 |
| `platforms/windows/tsf/CMakeLists.txt:8-9` | 注释称"没有注册导出"，与 `.def` 矛盾 | 已登记，未改动（不影响行为） |
| `AGENTS.md` §7 | 描述 `third_party/patches/*.patch` 流程，但该目录不存在且从未使用 | 已登记为文档与现实差距 |
| `README.md` | 阶段表自述"内部 Alpha、未注册、C 层未验证"，与代码一致 | 保持；本轮追加新能力 |

### 3.13 哪些组件存在许可证或公开分发阻断

| 项目 | 状态 | 依据 |
| --- | --- | --- |
| 42,356 行英文词表（Alt12Dicts 固定子集派生） | **仅限内部 Alpha**，跨法域再分发未获确认 | `docs/compliance/english-pilot-data.md` §5 |
| 中文词典（Terra 派生）、日文 Mozc 词典、法文 Hunspell 词典 | 各自保留原许可；分发审批未完成 | `docs/compliance/backend-zh-fr.md`、`backend-ja-corpus.md` |
| librime 构建树中的 `data/minimal/*`（`cangjie5.dict.yaml` 自述 GPL） | `build/` 整棵树**不得对外发布** | `README.md` §目录结构 |
| 五语言运行资源（`build/ime-resources/product`） | 内部暂存校验通过，公开候选包仍拒绝未审查 payload | `docs/testing/backend-resource-staging.md` |
| 项目自有代码 | AGPL-3.0-only，LICENSE 与决定记录完整 | `docs/product/license-decision.md` |
| 公开分发的整体门禁 | **BLOCKED**：`check_release.ps1` 仍报告 clean source、installer/toolchain、real-host、language-acceptance、证据绑定、数据授权等阻塞 | `docs/reports/ime-prediction-audit.md` §4 |

### 3.14 是否已有 CI 工作流与真实运行结果

**NOT_IMPLEMENTED。** `.github/` 不存在。所有"通过"结论都来自本机手工执行，
没有 CI 复跑。这是公开分发的独立阻塞项：没有第二个环境证明构建可复现。

### 3.15 最容易导致输入丢失、重复输入、越界编辑、崩溃或候选错配的风险

按严重度排序（均在源码中定位）：

| 风险 | 位置 | 说明 |
| --- | --- | --- |
| 高 | `composition_writer.cc:774-786` | `SetText` 失败标记 `kFailedPossiblyMutated` 且不重试。正确，但意味着宿主文本可能已被部分修改；调用方必须把该状态当"未知"处理 |
| 高 | `edit_session.cc:190-284` | 异步回调与主线程之间的目标/权限复检。任何新增写路径若绕过这些复检，会直接造成越界编辑 |
| 中 | `coordinator.cc:2044-2067` | UTF-8 转换失败必须拒绝整个计划，不能部分写入 |
| 中 | `backend_api.cc:792-797` | 后端丢弃 lexical 的 `row.score` 并重建最终分数；若未来新增特征，容易出现重复计分 |
| 中 | `engine/backend/context_text.h` | 只用 512 UTF-16 code unit 的字段局部上下文；跨未知词切断 trigram。上下文过短会降低预测质量，但不会损坏输入 |
| 中 | `learning_store` 无时间衰减 | 长期使用后早期偏好永久有效，可能出现"越用越偏"的排序 |
| 低 | `product_paths.cc` | 路径加固已拒绝 UNC/设备别名/ADS/reparse，但并发 DOS 设备重映射仍是残余边界 |
| 低 | `english_case.cc` | 混合大小写不补全；不会误改文本，只是能力缺口 |

## 4. 未实现 / 被阻断清单

**NOT_IMPLEMENTED：**

- Android IME 全部。
- 完整中日韩 TSF 行内组合编辑（当前是"宿主保留原文、显式选择后替换当前 token"）。
- 跨语言候选合并（本轮新增的 ranker 是原型，未接进后端会话）。
- 产品学习持久化启用。
- 模型目录、下载器、离线导入、模型安装状态机。
- 界面 i18n。
- CI。
- 安装器与签名。

**BLOCKED：**

- 当前源码对应包的真实宿主验收（候选可见性、跨宿主闭合）：缺 UAC/会话授权。
- 公开发行：数据再分发授权、干净源码证明、可重链接依赖包。

**待验证（做了但没有真实宿主证据）：**

- 13 个 profile 的物理键盘矩阵。
- 简繁输出、法文完整形态生成、日文母语重写、韩文词尾/自动空格。
- 神经重排器在真实输入法场景的延迟（只有构建级验证，未进产品路径）。

## 5. 本轮新增代码切片

见 `docs/reports/multilingual-intelligence-slice.md`：locale/profile 注册表、
有界自适应排序与学习事件语义，以及实际测试结果。

## 6. 建议的阶段计划

| 阶段 | 唯一主目标 | 完成定义 |
| --- | --- | --- |
| 下一轮（D） | 把 `LearningEventModel` 的语义接到持久化，且产品路径打开学习 | 迁移、原子写、隐私过滤、清除、重启后恢复全部有可失败测试；真实 TSF 产品回归仍绿 |
| D 之后（E1） | 后端会话支持多语言候选合并 | 一个会话可持有用户语言 + 至多一个检测语言；`engine/ranking` 接进 `backend_api.cc`；混合语言稳定性测试 |
| E2 | 中文多变体身份落到资源 | `zh-Hans-CN` / `zh-Hant-TW` / `zh-Hant-HK` / `yue-Hant-HK` 各自有独立获批数据；不再用简繁转换冒充地区支持 |
| E3 | Spanish / German 最小实现 | 各自至少一个真实输入路径 + 词库 + 排序 + 回归；否则保持 `kNoProfile` |
| F | 模型目录与离线导入 | 元数据默认可见；权重默认不下载；SHA-256 校验；删除模型不动偏好；无网络 I/O |
| G | Android 最小 IME | `InputMethodService` + 候选栏 + 不申请 INTERNET 权限；共享语言核心 |

本报告的结论不因新增文档数量而改变：**本轮完成的是一个可编译、可测试、可失败的核心切片，
不是完整输入法交付。**
