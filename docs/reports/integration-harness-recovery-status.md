# 集成测试恢复状态（事故后）

本文只记录**测试能力的恢复进度**，不记录事故经过：事故经过与保全清单在各
`EpoCanva-preserve-*` 目录内，本仓库不复制它们。

损失范围已确认为**一个文件**：`tools/rime_english_completion/main.cc`（现 0 字节，
已按空文件纳入快照提交 `0c4bc62`，使缺失在历史中可见）。
`engine/`、核心单测 `tests/english_core/main.cc`、三套夹具、两份 CMake 判定包装件、
方法与期望文档、以及事故前的具名运行日志全部存活。

## 0. 判据口径

- **本轮"通过"只指新工程** `tools/rime_english_recovery`（CTest 前缀 `recovery.*`）。
  原工程 `tools/rime_english_completion` 的注册与 `--mode` 名一律保留、未修改、未删除，
  它是"还有哪些行为待恢复"的记录，不是当前可运行的测试。
- 状态取值只有五种：`已从新源码编译验证` / `实现中` / `尚未恢复` /
  `仅有事故前日志证据` / `原本就未完成`。
- **"日志曾证明通过"绝不写成"源码已恢复"。** 反过来也一样：恢复目标不是复刻旧文件的
  字节数、行数或断言总数，而是覆盖同一组可观测行为。

## 1. 行为能力状态表

| 能力 / 旧注册名 | 行为要求 | 源码状态 | 证据 |
| --- | --- | --- | --- |
| 原文与前缀补全<br>`english.completion.prefix_candidates_and_commit` | 候选 0 恒为原文；补全按 weight 降序、同分 ASCII 升序；与原文相同的行不重复；预算 1+7；逐键 preedit 文本与字节长度；未选择时 Return 提交原文；显式选择后提交整词；退格重算；Escape 无残留；双 Session；词表只读一次 | **已从新源码编译验证**（新名 `recovery.completion.prefix_candidates_and_commit`，199 条断言 0 失败） | 新：`build/english-recovery/evidence-completion.log`；旧：`build/p23-verify/evidence-completion.log` |
| 候选来自真实引擎（交叉注入）<br>`…positive_mode_fails_on_baseline_engine`、`…negative_mode_fails_on_english_engine` | 同一程序换 DLL 必须给出不同结论：正向在 baseline 上 exit 20，负向在 english 上 exit 18，两者都不得打印成功标记 | **已从新源码编译验证**（`recovery.completion.positive_mode_fails_on_baseline_engine`、`recovery.completion.negative_mode_fails_on_english_engine`） | 新：`build/english-recovery/evidence-cross-baseline.log`、`evidence-absent-on-english.log` |
| 组件缺失时引擎本身仍健康<br>`english.completion.negative_baseline_engine` | `find_module()` 返回 NULL；候选 **0 项**（不是 1 项）；组合、preedit 与 Return 提交照常 | **已从新源码编译验证**（`recovery.completion.negative_baseline_engine`，11 条断言） | 新：`build/english-recovery/evidence-absent-baseline.log` |
| 测试必须能失败 | 断言一条夹具证明不存在的候选，程序必须以 14 退出且不打印成功标记 | **已从新源码编译验证**（`recovery.completion.suite_can_fail`） | 新：`build/english-recovery/evidence-self-check-fail.log` |
| 参数与模式校验 | 缺参数 exit 2；未知模式 exit 2；run-root 守护名不符 exit 6 且不删除任何东西 | **已从新源码编译验证**（`recovery.completion.rejects_unknown_mode` + 手工三条） | 新：`build/english-recovery/evidence-unknown-mode.log`、`evidence-noargs.log`、`evidence-relative.log` |
| 资源按公开 API 契约释放 | `get_/free_` 逐对配平，未配平则 exit 16；finalize 后不再触碰诊断面且 `create_session()` 被拒 | **已从新源码编译验证**（实测 status 27/27、context 27/27、commit 3/3） | 同上 completion 日志 |
| ASCII 大小写映射与原文保真<br>`english.completion.case_preservation_and_mapped_commit` | `lowercase`/`initial-capital`/`uppercase` 三形状可补全并按原形状渲染；`mixed` 与整词命中只出原文；`nomatch/mixed/notalpha/toolong` 四桶分离；`compl == cl+ci+cu`；`shift+h` 必须得到小写 `h` | **尚未恢复** | 仅有事故前日志证据：`build/p23-verify/evidence-case-preservation.log`；判据 `docs/testing/rime-english-completion.md` §6.1、`tests/fixtures/english_completion/README.md` §5.1 |
| 词形与技术文本保护<br>`english.completion.token_boundaries_and_protection`、`…negative_bad_lexicon_illegal_form` | 分隔符前缀（`don'`/`co-`/`can'`/`re-`）继续补全；数字与 11 个技术标记整词原样返回；非法词形是**独立状态名**；逐帧 `compl` 增量为 0 而原因桶 +1；`whole==queries, part==0`；65 字节不裁字节 | **尚未恢复** | 仅有事故前日志证据：`build/p23-verify/evidence-token-protection.log`、`evidence-bad-lexicon-illegal-form.log`；判据 §6.2、README §5.2 |
| 空格与连续多词输入<br>`english.completion.space_boundaries_and_multi_word_commit` | A/B/C 三规则；每步同时断言 handled、`get_input()` 原串、菜单、提交队列、`space_case_*` 计数；高亮≠选择；Ctrl/Alt/Super/release 不消费；四条恒等式 | **尚未恢复** | 仅有事故前日志证据：`build/p25-verify/evidence-space.log`；判据 §6.4、README §5.3 |
| 词表负向路径<br>`…negative_missing_lexicon`、`…bad_lexicon-duplicate`、`…bad_lexicon-illegal` | 四种失败各有 `attempts/ok/fail/entries/errline` + 状态名组合，且都只出原文 1 项；失败结果同样被缓存（`attempts=1`） | **尚未恢复**（新工程目前只复制正向夹具，负向夹具 `lexicon_bad_*.tsv` 与运行时改名规则未接） | 仅有事故前日志证据：`build/p23-verify/evidence-missing-lexicon.log` 等；判据 §8 表 |
| pilot 真实词表<br>`english.pilot.real_lexicon_candidates_commit_and_protection`、`…output_matches_the_pinned_manifest` | 42,356 行上同一套断言；`scale.*` 只打印不判定；产物哈希对上钉住清单 | **尚未恢复**（词表与清单在位：`build/english-pilot/epocanva_en_pilot.tsv`、`data/manifests/english-pilot.sources.lock.json`；第二条用例本身与夹具无关，仍由旧 CMake 注册） | 仅有事故前日志证据：`build/p25-verify/evidence-pilot.log`；判据 §6.3、§7 |
| P2.6 拼写建议<br>`english.completion.spelling_suggestions_and_suppressions` | 逐键一帧报十三具名状态之一；建议与补全互斥；`spell` 注释列区分二者；预算 ≤3/菜单 ≤8；三种大小写；退格重算；双 Session；词表不可读必须报"加载失败" | **尚未恢复**。整段实现文本仍在 `EpoCanva-preserve-2026-10-19T1652/incident-evidence/build/p26-verify/patch_spell_mode.py`（约 840 行插入文本），**但外层骨架（模式枚举、参数校验、dispatch、`EnglishSurface`、候选注释列采集）不在其中**；且该脚本以截断式写法落盘，禁止执行 | 仅有事故前日志证据：`build/p26-verify/mode-spelling.log`、`timing.txt`；判据 `tests/fixtures/english_spelling/README.md` |
| pilot-spelling 性能模式<br>`english.pilot.spelling_suggestion_cost_a_b_on_pilot_data` | 13 输入在开关两侧各跑一遍，逐键耗时分布，off 侧拼写计数零变化 | **原本就未完成**：补丁脚本崩溃于写入之前，该模式从未进入过任何一次绿色扫描（13/13 与 15/15 日志均无它）。它**不属于**事故前必须恢复的绿色基线 | `check_restored.py` 将其列为 FORBIDDEN；`patch_pilot_mode.py` 自述崩溃原因 |

## 2. 与新工程有关的两个事实

1. `tools/rime_english_completion/CMakeLists.txt`（mtime 2026-10-19 16:40）在 main.cc 已为
   0 字节之后被改过，其中 `pilot-spelling` 的注册（第 429-437 行）**先于实现存在**。
   因此旧工程当前无法构建成功；本轮不修改它，只在此登记。
2. 新工程复用存活的 `tests/english_completion/assert_exit.cmake`（精确退出码 +
   marker 策略），未新增第二种判定口径；也复用存活的
   `tests/fixtures/english_completion/`，夹具只读。

## 3. 下一步顺序（每步一个可编译、可判定的增量）

`case-preservation` → 三条词表负向 → `token-protection` → `space-commit` →
`spelling`（先只读判定 `patch_spell_mode.py` 的插入文本，外层骨架由新工程补齐）→
`pilot-lexicon` → 最后才是 `pilot-spelling`（新增，非恢复）。
