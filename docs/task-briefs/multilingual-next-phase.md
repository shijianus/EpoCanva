# 下一阶段任务书 — 阶段 D：把学习事件语义接入持久化并打开产品学习

## 0. 交付给谁

本文件可直接交给下一轮 CLI Agent。它假设你**没有**本轮对话上下文，
所以所有必要事实都写在这里；不要依赖记忆或上一轮总结。

## 1. 起点事实（需自行复核，不得直接采信）

| 项目 | 值 |
| --- | --- |
| 仓库根 | 工作目录本身 |
| 起点 HEAD | `84788168f89fe8467492d4b614828e530aa91845` |
| 分支 | `feat/offline-multilingual-backend` |
| 本轮结束时工作区 | 有未提交改动（本轮新增 engine/locale、engine/ranking、测试与文档） |
| 远端 | `origin` = https://github.com/shijianus/EpoCanva.git |
| 参考 `origin/main` | `5c0f11b1fa94c965e1a00f920272860c2d20ff73` |
| 工具链 | Windows x64，VS2022 Professional，CMake 3.31.6-msvc6（VS 随附，不在 PATH），Python 3.14 |
| CI | **不存在**，所有结论来自单机手工执行 |

开工前必须执行：

```bash
git rev-parse HEAD
git branch --show-current
git status --short
git diff --name-only
```

若 HEAD 已变化，先读 `git log --oneline -10` 与 `docs/reports/` 中最新的报告，
以真实代码为准。

## 2. 本轮已完成（阶段 B）

### 代码

- `engine/locale/language_tag.{h,cc}`：BCP 47 子集解析/规范化/
  RFC 4647 lookup 匹配/默认文字/RTL/七语言优先级。
- `engine/locale/locale_profile.{h,cc}`：输入 profile 注册表，
  三级方案实现状态（`kImplemented`/`kUnverified`/`kAbsent`），
  证据引用强制，歧义返回而非猜测，8 个内置 profile。
- `engine/ranking/learning_events.{h,cc}`：学习事件语义（显式选择/拒绝/被替代/
  手动词条），代际与归因校验、隐私拒绝、饱和、半衰期衰减、容量上限。
  **没有按键/提交/退格事件类型。**
- `engine/ranking/adaptive_ranker.{h,cc}`：有界确定性排序，
  特征区间、原样硬分层、语言锁存、去重、拒绝而非截断。
- `tests/locale_registry`、`tests/adaptive_ranking`、CMake 注册。
- `engine/language_packages` 的语言身份与优先级改为委托 `engine/locale`。
- `engine/include/epocanva_backend.h` 追加 `ES=6`/`DE=7` 身份常量（后端仍拒绝）。

### 文档

- `docs/reports/multilingual-intelligence-audit.md`（15 项逐条审计 + 证据等级）
- `docs/reports/multilingual-intelligence-slice.md`（真实命令与退出码）
- AGENTS.md §1/§2、`docs/product/requirements.md`、`docs/architecture/overview.md`、
  `docs/security/threat-model.md` 修订
- ADR 0005–0010

### 测试结果（本轮实际执行）

```text
cmake --preset vs2022-x64-debug                          exit 0
cmake --build build/vs2022-x64-debug --config Debug       exit 0，无 warning
ctest --test-dir build/vs2022-x64-debug -C Debug          exit 0；72/72 通过，1 项显式跳过
```

新增 16 条：`locale_registry.*` 3 条、`adaptive_ranking.*` 13 条。
负向控制已验证：把 `kSelectionGain` 改为 0 后 `adaptive_ranking.learning` 真实转红。

## 3. 本轮明确没有做

- ranker **没有**接进 `epocanva_backend` 或 TSF 产品路径。
- `LearningEventModel` **只在内存**，没有持久化、迁移、崩溃恢复。
- `engine/locale` 目录与 `tools/backend/stage_resources.py` 的 13 个 profile
  **没有合并**，存在两套清单分叉。
- Spanish/German 无引擎；中文繁体的三种身份无数据。
- 无模型目录/下载器/领域路由；神经重排器仍不接产品。
- 无 Android、无 CI、无安装器。

## 4. 下一阶段唯一主要目标

**把 `engine/ranking` 的学习事件语义接入一条真实的本地持久化路径，
并在产品设置中提供可用的学习开关与清除入口。**

不允许把本阶段扩大为"顺便接多语言合并"或"顺便做模型目录"。那些是后续阶段。

## 5. 必须完成的定义（Done）

1. **数据模型合并**：`LearningStore` schema 升级到 v3，包含
   - `profile_id` / `language` 身份键；
   - 正向与负向摘要；
   - `last_update_ticks` 与衰减元数据；
   - `manual` 标志（用户手动词条不被容量淘汰）；
   - schema version；
   - **不新增**原始击键、宿主应用名、窗口标题或字段内容字段。
2. **迁移**：v1/v2 文件可读并升级为 v3；升级后重新序列化；损坏文件不部分覆盖现有数据。
3. **原子写与恢复**：沿用 `windows_learning_repository.cc` 的临时文件 + 刷盘 + 重命名；
   增加启动时对损坏文件的隔离处理（重命名到 `.corrupt` 而不是静默删除）。
4. **隐私**：每次读写前重新查询字段权限；敏感/未分类字段既不写入也不作为排序输入。
5. **衰减**：持久化时间戳，加载后按当前 tick 衰减；tick 单位在文档中定义。
6. **界限**：新字段不得突破现有 256 KiB 与条目上限；超限时确定性淘汰。
7. **清除**：`Clear()` 与"关闭学习"是两个独立操作，各有 UI 入口；
   关闭学习立即停止新写入，既有偏好仍可用于排序。
8. **产品接线**：`unified_engine_backend.cc` 不再固定 `learning_enabled = 0`；
   但**只有在**字段权限锁覆盖整个 worker 调用链之后才允许打开。
   若该条件无法证明，保持关闭并在报告中写明，不得用测试替身冒充。
9. **测试**（必须能失败）：
   - 重启后合法学习状态仍存在；
   - 关闭学习后不再写新数据；
   - 清除后恢复默认排序；
   - 敏感字段从不落盘（用会失败的探针证明）；
   - 存储损坏时回退默认排序且不崩溃；
   - v2 → v3 迁移正确；
   - 普通退格不改变任何计数（结构上不存在该路径，需有回归断言）。
10. **回归**：现有 72 条 CTest 仍全部通过；真实 TSF 产品回归（若环境允许）不退化。

## 6. 不允许越过的边界

- 不执行 `git commit` / `push` / `tag` / PR。
- 不注册 TSF、不提权、不安装、不改系统配置。
- 不下载任何模型权重。
- 不修改 `third_party/` 下任何文件。
- 不给 `engine/ranking` 增加文件 I/O；持久化只能在 `engine/data_store`。
- 不为了让测试通过而放宽断言、删除失败用例或吞掉异常。
- 不把 `LearningEventModel` 的隐私判断从"拒绝未分类"改成"默认允许"。
- 不一次性重写已验证的 Windows TSF 编辑路径。
- 不在无证据的情况下宣称产品学习已启用。

## 7. 建议的实施顺序

1. 先写 v3 数据模型与迁移测试（红）。
2. 实现序列化/反序列化与迁移（绿）。
3. 接入衰减与手动标志，补测试。
4. 接 `LearningEventModel` → `LearningStore` 的适配层（单向，不反向依赖）。
5. 产品接线：先只读（加载并参与排序），再写（记录事件），最后才谈默认开关。
6. 全量构建与 CTest；记录真实退出码。

## 8. 验收时会检查的证据

- `docs/reports/` 新增一份阶段报告，含实际命令、退出码、通过/失败/跳过数量。
- 至少一个负向控制：故意破坏某条不变量，确认对应测试转红，再恢复。
- 文件清单完整，包含每一个新增/修改路径。
- 明确区分：已实现 / 已测试 / 待验证 / 被阻断 / 未开始。

## 9. 已知阻塞（不要假装解决）

- 当前包的真实宿主验收需要 UAC 授权与新的 C 层会话；没有授权就标 BLOCKED。
- 公开分发受数据授权（42k 词表、中日词典、语料）阻塞。
- 没有 CI，无法证明第二个环境可复现。

## 10. 一句话完成定义

**学习事件能跨进程重启存活、能被用户关闭与清除、敏感字段从不落盘，
且现有 72 条测试与产品回归全部保持通过。** 少任何一条都不算完成。
