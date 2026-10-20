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
| ASCII 大小写映射与原文保真<br>`english.completion.case_preservation_and_mapped_commit` | `lowercase`/`initial-capital`/`uppercase` 三形状可补全并按原形状渲染；`HeL`/`hE`/`hEL` 混合仅原文；`Hello` 整词命中是**去重**而非拒绝（`nomatch=1` 而 `rejected` 不动）；单个 `H` 归 initial-capital；`HEL` 退格回 `HE` 仍按全大写重算；默认 Return 提交原形状；显式选择提交 `HELLO`；提交二次读取为空；A=`HE` 与 B=`he` 互不污染；shift+`h` 实测得到小写 `h`（事件形状是被断言的事实）；整套形状矩阵只读词表一次 | **已从新源码编译验证**（新名 `recovery.case.preservation_and_mapped_commit`，466 条断言 0 失败；终局 `queries=29 with_index=29 rejected=3 mixed=3 notalpha=0 toolong=0 illegal=0 delimited=0 nomatch=1 cl=43 ci=67 cu=30 compl=140 attempts=1 whole=29 part=0 spell=0`，`api.balance status 42/42 context 42/42 commit 6/6`，`xl_ctor=4 xl_dstr=4 live=0`）。**范围声明**：只覆盖 52 个 ASCII 字母的四种形状，**不是** Unicode case folding、区位大小写或句首自动大写；按键走 `process_key()` 的 API 层，**不是** Windows 键盘适配 / TSF 层验证 | 新：`build/english-recovery/evidence-case.log`；旧：`build/p23-verify/evidence-case-preservation.log`；判据 `docs/testing/rime-english-completion.md` §6.1、`tests/fixtures/english_completion/README.md` §5.1 |
| 大小写组必须能失败 | 断言"同样四个词、次序错误"的菜单须以 14 退出且无成功标记；若菜单检查退化为只比集合，这一条会继续假装绿 | **已从新源码编译验证**（`recovery.case.suite_can_fail_on_menu_ranking`，实测 `[index 1]: got "Hello"`、`[index 2]: got "Help"`，11 checked / 2 failed） | 新：`build/english-recovery/evidence-case-self-check-fail.log` |
| 词形与技术文本保护<br>`english.completion.token_boundaries_and_protection` | `don'`/`Don'`/`DON'`/`co-`/`CO-`/`can'`/`re-` 作为**尾随分隔符前缀**继续补全并在选中后提交整词；`word-` 是可查询前缀但词表无行（计 no-match 而非拒绝）；数字与 11 个技术标记（`@ . _ : / \ ? & = # % +`）从决定字节起**每帧**塌成 1 项原文、`compl` 增量 0、`rejected` 恰好 +1、并命名 `protected`；`rock'n'roll`/`'hello`/`co--operate` 命名 `illegal-word-form`（与 protected、与 `mixed` 都是不同桶）；`Co-Operate`/`DoN't` 记 mixed 而非词形非法；`hel→hel@→BackSpace` 恢复帧恰好重发 3 条补全、`he2/he.→BackSpace` 重发 6 条；取消与跨 Session 不留残余保护；64 字节仍被查询、65 字节被拒绝且 `preedit`/`get_input()`/提交都逐字节保留 65 字节 | **已从新源码编译验证**（新名 `recovery.token.boundaries_and_protection`，1889 条断言 0 失败；终局 `queries=389 with_index=389 rejected=94 orig=389 compl=285 cl=210 ci=57 cu=18 delimited=90 nomatch=202 mixed=10 notalpha=63 toolong=2 illegal=19 protected=63 nonascii=0 unsup=0 whole=389 part=0 spell=0`，`api.balance status 446/446 context 446/446 commit 27/27`）。**范围声明**：`notalpha == protected + nonascii + unsup` 且 `nonascii == 0`，因此本组**只是 ASCII 证据**，不含任何非 ASCII 输入；`stop_offset`/长度一律为原始输入的 UTF-8 **码单元下标**（本组输入全 ASCII，故等于字节下标），不是字素位也不是屏幕列 | 新：`build/english-recovery/evidence-token3.log`（逐事件 `ev=` 上报）、控制组 `evidence-token-self-check-fail.log`；旧：`build/p23-verify/evidence-token-protection.log`；判据 §6.2、README §5.2 |
| 词表数据加载负向<br>`…negative_bad_lexicon_illegal_form`、`…missing_lexicon`、`…bad_lexicon-duplicate`、`…bad_lexicon-illegal` | 四种加载失败各有 `attempts/ok/fail/entries/errline` + 状态名组合；`illegal-word-form` 作为**加载状态名**与 `illegal-word` 区分 | **尚未恢复**——本轮未对加载负向作任何声明 | 仅有事故前日志证据：`build/p23-verify/evidence-bad-lexicon-illegal-form.log` 等；判据 §8 表 |
| 空格与连续多词输入<br>`english.completion.space_boundaries_and_multi_word_commit` | A/B/C 三规则；每个事件同时断言 handled、`get_input()` 原串、preedit、菜单、真实 commit；**高亮不等于接受**（`highlight_candidate(1)` 确实把高亮移到 index 1，空格仍提交 `hel `）；显式选择后队列仍空、组合仍在、菜单因 `Segment::Close()` 变为 0 项，随后空格提交 `hello ` 且只提交一次；空组合连按两次空格得到两个空格；连续多词 transcript 只由 `get_commit()` 返回值拼接；`don'` 默认提交部分词、选择后提交 `don't `；邮箱整串逐字节提交后下一个普通词恢复补全；`HEL ` 之后的小写词不继承形状；Escape 后空格不复活被取消文本；Ctrl/Alt/Super/release 空格不消费且组合原样保留；shift+空格**不是**阻塞修饰键；空组合 BackSpace 交回宿主；双 Session 交错不串扰；四条计数恒等式 | **已从新源码编译验证**（新名 `recovery.space.commit_and_multiword`，299 条断言 0 失败；实测 `pk_events=97 space_seen=24 refused=4 case_a=15 case_b=2 case_c=3 commits_issued=17 spaces_emitted=20`——`seen=a+b+c+refused`、`emitted=a+b+c`、`commits=a+b`、`processor_key_events==注入键数` 全部闭合；`api.balance status 103/103 context 103/103 commit 20/20`；`sp_ctor=4 sp_dstr=4 live=0`；transcript `"hel Hel HEL hel hello   Hello world don' don't user@example.com hel HEL he  hel hel app he "`）。**范围声明**：按键经 `RimeApi::process_key()`，**不是** Windows TSF/宿主文档层验证；空组合 BackSpace 只是"本策略不消费"的回退断言；本组不引入任何跨已提交文本的删除或撤销 | 新：`build/english-recovery/evidence-space3.log`、控制组 `evidence-space-self-check-fail.log`；旧：`build/p25-verify/evidence-space.log`；判据 §6.4、README §5.3 |
| 空格组的可失败性 | 断言"高亮后空格提交了 `hello `"这一被禁止的行为，必须以 14 失败且无成功标记 | **已从新源码编译验证**（`recovery.space.suite_can_fail_on_highlight`，实测 `got "hel "`） | 同上控制组日志 |
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

## 3. 顺序与已完成批次（每步一个可编译、可判定的增量）

已完成：批次 1 = 公共框架 + `completion`（6 条）；批次 2a = `case-preservation`（8 条）；
批次 2b = `token-protection`（10 条）；批次 3a = `space-commit`（12 条）。

待做：三条词表**加载**负向 →
`spelling`（先只读判定 `patch_spell_mode.py` 的插入文本，外层骨架由新工程补齐）→
`pilot-lexicon` → 最后才是 `pilot-spelling`（新增，非恢复）。

### 批次 2a 的一处披露

`case` 组那一次提交里，我先在 `recovery_test_support.h/.cc` 写入了**四个**新
`Mode` 名（含 `token-protection` / `token-self-check-fail`），而 `main.cc` 的派发表当时只
登记了 `case` 的两个。那次提交的树因此接受 `--mode token-protection` 这个名字、却在
派发处落到"该模式在此构建中没有实现"，返回 2。`case` 组自身的 8 条测试不受影响，也
没有任何断言被放宽；该不一致在批次 2b 实现 token 组后自然消失，历史不重写。

### 批次 3a 的几处披露（全部是测试自身的错误，产品行为未改）

1. `read_commit`/`expect_no_commit` 初版在事件之后再调用一次 `get_commit()` 并把那次读取
   当作事件结果；而 `SendKey` 已经在每个事件后读走并**消费**了提交（`get_commit()` 消耗性），
   于是产生 20 条假失败——引擎侧每次空格的实际提交在 `ev=` 行里都是正确的
   （`"hel "`、`"Hel "`、`"HEL "`、高亮后仍是 `"hel "`、选择后 `"hello "`）。
   改为断言事件自身的观测值，第二次读取只用于"消费后不重复"。
2. 我原先断言"显式选择之后菜单仍是那 4 项"。这与上游不符：
   `ConcreteEngine::OnSelect()` 会 `Segment::Close()` 并 `Forward()`
   （`src/rime/engine.cc:259-270`），选择后**不再提供菜单**。现按实际行为断言 0 项并写明理由；
   该步骤其余断言（提交队列为空、`get_status()` 仍在组合、`get_input()` 仍为 `hel`）保留。
3. 常量笔误：`kMenuHello` 被写成小写 `"hello"`，而被键入的是 `Hello`，造成 1 条真失败。
4. `HighlightCandidate` 里用了一个不存在的阶段码 `kGetStateFailed`，编译期即暴露，改用
   `kGetStatusFailed`。
5. 核查过程中我曾用一条被 `head -40` 截断的 grep 误判"CMake 丢失了用例注册"，实际文件
   一直注册着 10 条；本轮改为直接读取文件逐项核对后才动手。旧日志（修复前的
   `evidence-space.log`、`evidence-space2.log`）一律保留，未被最终日志覆盖。
