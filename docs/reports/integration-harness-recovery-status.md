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
  **【批次 7 更正，2026-10-22】**该目录的两个文件此后已按授权删除（0 字节的 `main.cc`
  与失效且先注册后实现的旧 `CMakeLists.txt`），目录内只剩指向新正式入口的 `README.md`。
  "还有哪些行为待恢复"的职责由本文 **§1A 覆盖矩阵**接管，旧注册名仍逐条列在矩阵里。
  运行方式改为 `tools/test_english_integration.ps1`，不要再手工拼 cmake/ctest 命令。
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
| 词表数据加载负向<br>`…negative_missing_lexicon`、`…bad_lexicon-duplicate`、`…bad_lexicon-illegal`、`…negative_bad_lexicon_illegal_form` | 四类条件各自必须是**可区分的三元组**（候选列表 + 加载计数 + 状态名）：`file-not-found/errline 0`、`duplicate-word/3`、`illegal-word/3`、`illegal-word-form/3`，且一律 `attempts=1 ok=0 fail=1 entries=0`；`config_missing_path` 必须为 0（证明 schema 确实指向了词表，坏的只是文件）；每帧只有原文 1 项、`with_index/compl/rejected/nomatch` 增量全为 0（"失败加载"既不是补全也不是"查了没中"）；Return 仍逐字节提交原文且二次读取为空；六个探测键之后 `attempts` 仍为 1（**失败被缓存，不逐键重读**，且不改动文件冒充热重载）；每例独立进程、独立运行目录，词表只在本次运行的 `shared/lexicon/epocanva_test_lexicon.tsv` 上暂存或移除，`tests/fixtures/` 全程只读（实测暂存字节数等于所用夹具：31/30/24，控制例 429）；`Session` 销毁后 `live=0 constructed==destroyed`，`api.balance status 9/9 context 9/9 commit 1/1` | **已从新源码编译验证**（新名 `recovery.lexicon.negative_missing_file` / `…negative_duplicate_entry` /
`…negative_illegal_word` / `…negative_illegal_word_form`，各 79 条断言 0 失败、退出 0）。**未宣称**：空文件、空行、缺字段、多字段、权重非法/溢出、词太长这些其余加载状态仍未接测试；非 ASCII 词条字节只在核心单测里覆盖，引擎路径不可达 | 新：`build/english-recovery/evidence-3b-{missing,duplicate,illegal,illegal-form}-r2.log`；旧：`build/p23-verify/evidence-missing-lexicon.log`、`evidence-bad-lexicon-*.log`；判据 §8 表、`tests/fixtures/english_completion/README.md` §6、`engine/languages/en/english_lexicon.cc` |
| 空格与连续多词输入<br>`english.completion.space_boundaries_and_multi_word_commit` | A/B/C 三规则；每个事件同时断言 handled、`get_input()` 原串、preedit、菜单、真实 commit；**高亮不等于接受**（`highlight_candidate(1)` 确实把高亮移到 index 1，空格仍提交 `hel `）；显式选择后队列仍空、组合仍在、菜单因 `Segment::Close()` 变为 0 项，随后空格提交 `hello ` 且只提交一次；空组合连按两次空格得到两个空格；连续多词 transcript 只由 `get_commit()` 返回值拼接；`don'` 默认提交部分词、选择后提交 `don't `；邮箱整串逐字节提交后下一个普通词恢复补全；`HEL ` 之后的小写词不继承形状；Escape 后空格不复活被取消文本；Ctrl/Alt/Super/release 空格不消费且组合原样保留；shift+空格**不是**阻塞修饰键；空组合 BackSpace 交回宿主；双 Session 交错不串扰；四条计数恒等式 | **已从新源码编译验证**（新名 `recovery.space.commit_and_multiword`，299 条断言 0 失败；实测 `pk_events=97 space_seen=24 refused=4 case_a=15 case_b=2 case_c=3 commits_issued=17 spaces_emitted=20`——`seen=a+b+c+refused`、`emitted=a+b+c`、`commits=a+b`、`processor_key_events==注入键数` 全部闭合；`api.balance status 103/103 context 103/103 commit 20/20`；`sp_ctor=4 sp_dstr=4 live=0`；transcript `"hel Hel HEL hel hello   Hello world don' don't user@example.com hel HEL he  hel hel app he "`）。**范围声明**：按键经 `RimeApi::process_key()`，**不是** Windows TSF/宿主文档层验证；空组合 BackSpace 只是"本策略不消费"的回退断言；本组不引入任何跨已提交文本的删除或撤销 | 新：`build/english-recovery/evidence-space3.log`、控制组 `evidence-space-self-check-fail.log`；旧：`build/p25-verify/evidence-space.log`；判据 §6.4、README §5.3 |
| 空格组的可失败性 | 断言"高亮后空格提交了 `hello `"这一被禁止的行为，必须以 14 失败且无成功标记 | **已从新源码编译验证**（`recovery.space.suite_can_fail_on_highlight`，实测 `got "hel "`） | 同上控制组日志 |
| 加载失败组必须能失败 | 把**合法**的 54 行夹具放到同一路径，再走同一条"期望加载失败"的验证路径：健康菜单断言必须通过，而三条加载诊断断言（`failures=1`、`entries=0`、状态名 `duplicate-word`）必须因与实测 `ok/54` 不符而失败，退出码 14 且无成功标记 | **已从新源码编译验证**（`recovery.lexicon.suite_can_fail_on_valid_data`，实测 16 checked / 3 failed，失败项名字逐条指名"deliberately claimed …"） | 新：`build/english-recovery/evidence-3b-control-r2.log` |
| 一处实现观察（非本轮修复范围） | 重复词条的行号归因：`english_lexicon.cc:203-216` 在 `std::sort` **之后**相邻比对并报 `origin_line[order[i]]`，而 `std::sort` 不保证等值元素保持文件顺序，因此 `lexicon_bad_duplicate.tsv`（`hello` 在第 1 与第 3 行）报告的行号可能是 1 或 3；本轮实测为 3 | **未修改产品**：测试按夹具事实断言"行号 ∈ 重复对所处的两行"，并打印实测值；若希望行号归因确定，建议把重复检测改为按文件顺序比对（例如排序时携带原下标做 tie-break，或先线性扫一遍已见集合），属产品改动，等授权 | 观察来源：该文件源码；`build/english-recovery/evidence-3b-duplicate-r2.log` |
| pilot 真实词表<br>新名 `recovery.pilot.real_lexicon_reference_compared`、`…suite_can_fail_on_reference_ranking`、`…truncated_data_refused_before_the_engine`、`…absent_data_refused_before_the_engine`、`…output_matches_the_pinned_manifest` | 在**真实词表**上跑同一套断言，且预期值不得来自被测实现：候选由独立的整表线性扫描参考实现（`pilot_reference.{h,cc}`：全表匹配 → weight 降序、同分按规范词 ASCII 升序 → 按输入形状渲染 → 与显示文本去重 → 截到 1+7）逐帧比对，它不 include 也不调用 `prefix_completer`/`english_case`/`english_token`/`english_lexicon`；查询集 28 行覆盖单字母、2/3 字母、长前缀、整词、无命中、`'`/`-` 尾随前缀、initial-capital、uppercase、混合与非法词形、邮箱/URL/Windows 路径/下划线/版本号；**数据身份是被验证的事实而非假设**：`build/english-pilot/epocanva_en_pilot.tsv` 实测 502,333 字节、SHA-256 `6e46587d8029d8c9cf17b20d33983d051caa6e5e70a3da6573707d25400aae6b`、42,356 行合法词条，与清单 `output.{sha256,size_bytes,line_count}` 逐项相等，来源 `alt12dicts/2of12full.txt` @ `7bbe293425d48a1210bf17aaf156b28ade5c2de1`；权重是 2..12 的**跨词典佐证档位**，`weights_are_corpus_frequency: false`，因此本 pilot 是 Alt12Dicts 单文件子集的本地试验，**不是** ESDB/SCOWL 35/50/60 尺寸变体导出，也不是语料频率 | **已从新源码编译验证**（正向 `recovery.pilot.real_lexicon_reference_compared` 实测 **868 条断言 0 失败**、`RESULT PASS`、exit 0；引擎 `entries=42356` 与被验证行数相等、`attempts=1 ok=1 fail=0 errline=0` 状态名 `ok`（数百键仍只读一次）；终局 `queries=211 with_index=211 rejected=86 orig=211 compl=737 cl=624 ci=71 cu=42 delimited=67 nomatch=14 mixed=1 notalpha=71 protected=71 illegal=14 nonascii=0 unsup=0 toolong=0 whole=211 part=0 spell=0`，`processor_key_events==注入键数 248`；`api.balance status 252/252 context 252/252 commit 5/5`，两 Session 销毁后 `translator_live=0 space_processor_live=0`；默认 Return 提交原文、显式选择提交参考实现排名第一的整词、`co-` 空格得 `co- `、选择后得整词+一个空格、`user@example.com ` 逐字节整串提交后下一个词恢复补全；耗时分布只打印不判定：`keys=248 min=1.0us med=4.1us p95=254.9us max=318.6us mean=42.2us`，单机单次，**不是基准测试**）。**三种拒绝种类彼此可区分**：exit 22 = 预校验在引擎打开任何文件前拒绝（`lexicon_load_attempts` 仍为 0，实测理由 `size-mismatch`、`file-absent`，另有手工 `digest-mismatch`）；exit 14 = 预校验通过、引擎真的加载了 42k 行、某条预期与实测不符（控制组实测 `[index 1]: got "practicable"`、`[index 2]: got "practical"`，37 checked / 2 failed）；exit 18 = 那条"应当拒绝"的断言本身没成立（用未编译进身份预期的构建跑同一模式实证，见 §3 批次 4）。配置层另有三重拒绝：路径非绝对 / 文件名与清单不符 / 字节数或 SHA-256 与清单不符一律 `FATAL_ERROR`，**绝不静默不注册**。开关关闭时 `ctest --show-only=json-v1` 仍为 **17** 条且不含任何 pilot 名，开启时为 **22** 条；批次 5 把摘要辅助自己的 3 条（开启时 4 条）也注册进来，批次 6 再加 5 条 `recovery.spelling.*`，因此现为 **25 / 31** 条；17 条既有引擎用例与 5 条 pilot 用例的名称、模式与判定一字未改（批次 6 只在同一 `set_tests_properties` 名单尾部追加，并把 LABELS 增加 `spelling` 一项） | 新：`build/english-pilot-recovery/evidence-pilot-positive-r1.log`、`evidence-pilot-control-r1.log`、`evidence-pilot-truncated-r1.log`、`evidence-pilot-absent-r1.log`、`build/english-recovery/evidence-pilot-digest-control-r1.log`、`evidence-pilot-truncated-noexpectation-r1.log`；ctest 列表 `build/english-pilot-recovery/show-off-r1.json`(17)、`show-on-r1.json`(22)，运行结果 `ctest-on-pilotnew-r1.log`、`ctest-on-full-r1.log`(22/22)、`ctest-off-full-r1.log`(17/17)；身份三重独立印证：CMake `file(SHA256)`、`sha256sum`、`pilot_sha256.cc`（批次 4 时为该工程自实现的 SHA-256，先用 FIPS 180-4 空串/`abc`/56 字节三向量验过；批次 5 起改为调用 Windows CNG，见下一行）给出同一值；旧证据仅作历史：`build/p25-verify/evidence-pilot.log`；判据 §6.3、§7、`tests/fixtures/english_pilot/README.md` |
| 摘要辅助自身（测试工具，不是产品能力）<br>新名 `recovery.pilot_sha256.known_vectors`、`…negative_identity_kinds`、`…suite_can_fail_on_wrong_vector`、`…real_lexicon_digest_matches_the_manifest` | 身份校验依赖一个文件摘要，所以那个摘要的实现必须被单独检验，而且**期望值不得由被测实现产出**：空文件、`abc`、FIPS 180-4 的 56 字节样例句、55/56/57 填充边界、63/64/65 分组边界、整段 `0x00..0xFF` 二进制、以及被测实现的读块边界 65,535/65,536/65,537 字节共 15 个夹具，逐一与 python hashlib、GNU coreutils `sha256sum`、PowerShell `Get-FileHash` 三个外部实现算出的值比对；负向侧要求区分四种种类：文件缺失、目录（两者都必须报**打不开**且**不产出任何摘要**）、截断副本、同长度只改一字节（后两者必须报**摘要不同**而不是 I/O 失败）；控制组用一条故意写错的 `abc` 期望实证该组能失败 | **已从新源码编译验证**（批次 5：`pilot_sha256.cc` 的自实现 SHA-256 已删除，改为调用 **Windows CNG**（`bcrypt.dll` 的 `BCryptOpenAlgorithmProvider(SHA256)`/`CreateHash`/`HashData`/`FinishHash`，每个 NTSTATUS 都判、`BCryptGetProperty(HashDigestLength)` 必须恰好 32 字节否则拒绝渲染），二进制分块 64 KiB，句柄由三个 RAII 对象在任何返回路径上释放；实测 vectors **60 条断言 0 失败**、negatives **21 条 0 失败**、控制组 **2 checked / 1 failed** 且退出 14 无成功标记；真实 TSV 经 CNG 得 `6e46587d8029d8c9cf17b20d33983d051caa6e5e70a3da6573707d25400aae6b`（4 条断言 0 失败），与 `sha256sum`、配置期 CMake `file(SHA256)` 逐项相等。**范围声明**：`kReadFailed`/`kHashFailed` 两种分类在本轮**没有被运行时触发**（无故障注入），只在类型与命名上与之可区分；该辅助**只在 Windows 上成立**，本轮不宣称跨平台；身份固定也**不等于**来源作者签名验证） | 新：`build/english-recovery/cng-evidence-helper-vectors-r1.log`、`cng-evidence-helper-negatives-r1.log`、`cng-evidence-helper-control-r1.log`、`cng-evidence-helper-refusals-r1.log`（首轮，含我写错的 usage 串）、`cng-evidence-helper-refusals-r2.log`（最终二进制：守护名不符 6、未知模式 2、`--pilot-lexicon` 被不接收它的模式拒掉 2、相对路径被 `real-file` 拒掉 2、`…/rime-english-recovery-run/../x` 这种拼回去能过守护的写法同样被拒 6）、`build/english-pilot-recovery/cng-evidence-helper-realtsv-r1.log`、`cng-evidence-digest-mismatch-r1.log`（同长度改一字节的真实副本：`22 / digest-mismatch`、`0 failed assertion(s)`）、`cng-evidence-open-failed-r1.log`（ACL 拒绝读的真实副本：落到暂存层 `fixture.copy … Access is denied` 退出 6，仍不是 `digest-mismatch`）、`build/english-recovery/cng-evidence-imports-r1.log`（`dumpbin /IMPORTS`：`rime.dll` 只导入 `KERNEL32/USER32`，`bcrypt.dll` 只出现在三个测试可执行文件里）、`cng-ctest-off-r1/r2.log`(20/20)、`cng-ctest-on-r1/r2.log`(26/26)，最终一轮对着最终二进制重跑：`build/english-recovery/cng-ctest-off-final-r3.log`(20/20)、`build/english-pilot-recovery/cng-ctest-on-final-r3.log`(26/26)、`show-off-cng-r1.json`(20)、`show-on-cng-r1.json`(26) |
| P2.6 拼写建议<br>旧名 `english.completion.spelling_suggestions_and_suppressions`<br>新名 `recovery.spelling.suggestions_and_suppressions`、`recovery.spelling.commit_and_selection`、`recovery.spelling.negative_lexicon_unavailable`、`recovery.spelling.suite_can_fail_on_suggestion_ranking`、`recovery.spelling.suite_can_fail_on_labels` | 逐键一帧报十三具名状态**之一**（`summed == 本帧查询数`、`恰好一个状态计数移动`、且状态名与预期一致）；一次编辑只允许删除/插入/替换/相邻换位四种且不许合成第二次编辑；`spell` 注释列是 C API 唯一能区分建议与补全的东西；建议与补全每查询互斥（判据用**截断前**的 `match_count`）；预算 ≤3、菜单仍 ≤8；权重降序、同分按规范词 ASCII 升序；多条路径命中同一行只出现一次；`lowercase`/`initial-capital`/`uppercase` 三形状渲染，`mixed-case` 不猜；长度窗口 4..32 两端与两端之外；技术文本与带分隔符词形整词拒绝且**在"词是否已存在"之前**判定；退格按当前输入重算；默认 Return 保原文、默认 Space 保原文+一个空格、高亮不等于接受、明确选择后提交建议且只提交一次；Escape 不留残余；双 Session 不串扰；建议提交后下一个普通词照常补全；词表不可读必须报 `lexicon-unavailable` 而不是"没有建议" | **已从新源码编译验证**（新工程 `tools/rime_english_recovery/spelling_tests.{h,cc}`、`spelling_commit_tests.{h,cc}`、`spelling_negative_tests.{h,cc}`，共 5 条具名用例；门槛组实测 **4361 条断言 0 失败**、`frames=250 one-query=250 queries=250 max=1`、终局 `queries=257 with_index=257 orig=257 compl=358 cl=292 ci=39 cu=27 nomatch=59 mixed=7 protected=54 delimited=19 nonascii=0 unsup=0 toolong=0 whole=257 part=0 spell=16385`、`attempts=1 entries=16 errline=0 status ok`；提交组 **308 条 0 失败**，`pk_events=85 space_seen=11 case_a=6 case_b=4 case_c=1 commits_issued=10 spaces_emitted=11`（三条恒等式全部闭合）且 `sp_app=31 = l25 + i3 + u3`，transcript 只由 `get_commit()` 拼出：`"helohelo helo hellohello HELLO Hello user@example.com  hello hel wrld helo "`，`api.balance status 102/102 context 102/102 commit 13/13`；不可读组 **63 条 0 失败**：`attempts=1 ok=0 fail=1 entries=0 errline=3` 状态名 `duplicate-word`、`config_missing_path=0`、4 帧查询 = 4 次 `lexicon-unavailable`、`no_candidate=0`、`appended=0`、`variants=0`，菜单仍只有原文 1 项且 Return 逐字节提交 `helo`；两条控制组分别 **11 checked / 2 failed**（`[index 1]: got "hello"`、`[index 2]: got "help"`）与 **7 checked / 1 failed**（实测注释列只有一个空字段加一个分隔符，即补全项确实没有 `spell` 标签），退出 14 且无成功标记）。**期望值两条独立来源**：字面量菜单手抄自 `tests/fixtures/english_spelling/README.md` §4，逐帧状态与候选列表/次序由 `spelling_tests.cc` 内**反向一次编辑参考实现**（拿词表每一行与输入做长度差判定，不生成变体、不 include 也不调用 `spelling_suggester`/`prefix_completer`/`english_case`/`english_token`）现算，并要求它等于 README 的 `(权重,编辑)` 表；引擎菜单同时与两者比对。**范围声明**：`spelling_skipped_unsupported_byte == 0` 与 `spelling_skipped_non_ascii == 0` 是**不可达**而不是没测——`;` 之类字节不在 `speller/alphabet` 里，真实按键进不了组合区，该两态只在核心单测 case 36 门槛表覆盖，因此本组同样只是 ASCII 证据；`SpellingEdit` 标签在 C API 不可见（`RimeCandidate` 只有 text/comment），所以四类编辑的**种类**由参考实现与文档表断言，引擎侧断言文本/次序/预算/标签列；按键经 `RimeApi::process_key()`，不是 Windows TSF 宿主层验证；词表权重是手工档位，**不是**纠错概率；5 条用例全部跑手工夹具，**不依赖** pilot 词表；"功能关闭"由 `recovery.completion.*`/`case`/`token`/`space`/`pilot` 各条既有用例末尾的 `SpellingTotal == 0`（19 个 `spelling_*` 字段逐项求和）承担，本轮未新增空壳模式，`pilot-spelling` 未注册、未实现 | 新（pilot 关闭配置，逐条单跑最终二进制）：`build/english-recovery/spelling-ctest-off-final-gates-r1.log`、`…-final-commit-r1.log`、`…-final-unavailable-r1.log`、`…-final-rank-r1.log`、`…-final-label-r1.log`，全套 `spelling-ctest-off-all-r2.log`(25/25)；新（pilot 开启配置）：`spelling-ctest-on-all-r2.log`(31/31)、`spelling-ctest-on-group-r1.log`(5/5 逐条详单，数字与关闭侧逐项相同)，构建 `spelling-build-off-r4.log`、`spelling-build-on-r2.log`（均 0 error 0 warning），清单 `show-off-spelling-r1.json`(25)、`show-on-spelling-r1.json`(31)；过程中三份失败或未跑的日志原样保留且未被覆盖：`spelling-ctest-off-gates-r1.log`（ctest 参数）、`-gates-r2.log`（注释列拼接缺陷）、`spelling-ctest-off-commit-r1.log`（"No tests were found"，见批次 6 披露 1/2）；旧证据仅作历史：`build/p26-verify/mode-spelling.log`、`timing.txt`；判据 `tests/fixtures/english_spelling/README.md`、`engine/languages/en/spelling_suggester.{h,cc}`、`engine/rime_components/english/english_translator.cc:362-514` |
| pilot-spelling 性能模式<br>`english.pilot.spelling_suggestion_cost_a_b_on_pilot_data` | 13 输入在开关两侧各跑一遍，逐键耗时分布，off 侧拼写计数零变化 | **原本就未完成**：补丁脚本崩溃于写入之前，该模式从未进入过任何一次绿色扫描（13/13 与 15/15 日志均无它）。它**不属于**事故前必须恢复的绿色基线。**批次 7 复核**：成本试验的**核心**那一半 `english.core.spelling_cost_trial_on_pilot_lexicon` 已注册并通过（真实词表在位时）；**引擎**那一半仍未实现、仍未注册，也不迁入正式入口。见 §1A 第 16 / 16b 行 | `check_restored.py` 将其列为 FORBIDDEN；`patch_pilot_mode.py` 自述崩溃原因 |

**§1 表中各行的用例条数（"17 条"、"25 / 31"等）是那一行被写下时那一批的快照，不随后续批次回改。**
当前计数见 §1A 与 §4：批次 7 之后，pilot 关闭 **27** 条、pilot 开启 **33** 条，
拼写组从 5 条增至 7 条。数量不是恢复完整性的判据，判据是 §1A 的行为对齐。

## 1A. 覆盖矩阵（原能力 → 当前具名测试 → 层 → 状态）

状态取值：**A** 已恢复；**B** 必须恢复但仍缺失；**C** 原本未完成；
**D** 当前仅核心覆盖；**E** 额外待补测试。

矩阵按**可观测行为**逐条对齐，不按断言条数对齐。旧工程的 `main.cc` 已经不存在，
它每条用例内部究竟断言到什么颗粒度**无法从现存材料恢复**，因此"新用例断言更多"
不构成"恢复更完整"的证据：两边的对应关系只能由**判据文档**
（`tests/fixtures/*/README.md`、本文 §6–§10、`docs/testing/rime-english-completion.md`）
来定义，而判据文档全部存活。下表凡"层"列写"集成"的，都是真实 `process_key()` +
真实 `get_context()`/`get_commit()` 读数。

| # | 原能力 / 旧注册名 | 当前具名测试 | 层 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 前缀补全、候选 0 恒为原文、weight 降序同分 ASCII 升序、预算 1+7、逐键 preedit 文本与字节长度、默认提交原文、显式选择提交整词、退格重算、Escape 无残留、双 Session 隔离<br>`english.completion.prefix_candidates_and_commit` | `recovery.completion.prefix_candidates_and_commit` | 集成（另有核心 `TestQuerySets`/`TestOrdering`/`TestKLimits`/`TestFileLoads`） | A |
| 2 | 结论由实际加载的二进制决定，而非 `--mode` 字符串（交叉注入）<br>`english.completion.positive_mode_fails_on_baseline_engine`、`…negative_mode_fails_on_english_engine` | 同名 `recovery.*` 两条 | 集成 | A |
| 3 | 组件缺失时引擎本身仍健康：候选 **0 项**、组合与提交照常<br>`english.completion.negative_baseline_engine` | `recovery.completion.negative_baseline_engine` | 集成 | A |
| 4 | 词表不逐键重载（`attempts` 只在缓存未命中时移动）<br>（内建于第 1 条） | 第 1 条 + 四条词表失败 + `feature_toggle_ab` 显式断言两阶段 `attempts` 零增量 | 集成 | A |
| 5 | 资源按公开 API 契约逐对配平，未配平 exit 16；finalize 后诊断面不再被触碰<br>（内建于每条用例） | 全部引擎用例；本轮把 `schema_open()/config_close()` 也纳入同一配平面（`config n/n`） | 集成 | A |
| 6 | 参数与模式校验<br>`english.completion.rejects_unknown_mode` | `recovery.completion.rejects_unknown_mode`（未知模式 → 2） | 集成 | A |
| 6b | 缺参数 → 2；`--run-root` 守护名不符 → 6 且**不删除任何东西** | **无注册用例**，只有手工运行日志 | 手工 | **E**（应补成注册用例） |
| 7 | 测试必须能失败 | `recovery.completion.suite_can_fail`、`recovery.case.suite_can_fail_on_menu_ranking`、`recovery.token.suite_can_fail_on_protected_text`、`recovery.space.suite_can_fail_on_highlight`、`recovery.lexicon.suite_can_fail_on_valid_data`、`recovery.spelling.suite_can_fail_on_suggestion_ranking`、`…on_labels`、`…suite_can_fail_on_toggle_comparison`、`recovery.pilot.suite_can_fail_on_reference_ranking`、`recovery.pilot_sha256.suite_can_fail_on_wrong_vector` | 集成 + 辅助 | A（旧工程**没有**任何可失败性注册，属超出恢复范围的新增） |
| 8 | ASCII 大小写映射与原文保真（四形状、混合不猜、整词命中是去重、退格后按剩余形状重算、默认提交原形状、选择按形状提交、双 Session 互不污染、整套矩阵只读词表一次）<br>`english.completion.case_preservation_and_mapped_commit` | `recovery.case.preservation_and_mapped_commit` | 集成（另有核心 `TestCasePreservation`/`TestApplyCaseStyleWithDelimiters`） | A。**范围**：只有 52 个 ASCII 字母；Unicode case folding、区位大小写、句首自动大写均未实现，非恢复项 |
| 9 | 词形边界与技术文本保护<br>`english.completion.token_boundaries_and_protection` | `recovery.token.boundaries_and_protection` | 集成（另有核心 `TestTokenStructureTable`/`TestProtectionTransition`） | A |
| 9b | 非 ASCII 字节进不了组合区，引擎路径不可达 | `recovery.token.*` 里 `nonascii == 0` 是**不可达**证明而非覆盖 | 核心 `TestRejectedInputs`/`TestDictionaryEntryForms` | **D** |
| 10 | 空格三条规则、真实 commit 逐事件读取、修饰键不消费、空组合 BackSpace 交回宿主、双 Session 交错、四条计数恒等式<br>`english.completion.space_boundaries_and_multi_word_commit` | `recovery.space.commit_and_multiword` | 集成（另有核心 `TestSpaceKeyClassification`/`TestSpaceCaseTable`/`TestSpacePolicyConstants`） | A |
| 11 | 四类词表**加载**失败各自可区分（候选 + 加载计数 + 状态名三元组）<br>`…negative_missing_lexicon`、`…bad_lexicon-duplicate`、`…bad_lexicon-illegal`、`…negative_bad_lexicon_illegal_form` | `recovery.lexicon.negative_missing_file` / `…_duplicate_entry` / `…_illegal_word` / `…_illegal_word_form` | 集成 | A |
| 11b | 其余加载状态：空文件、空行、缺字段、多字段、权重非法/溢出、词过长 | 无引擎用例 | 核心 `TestMalformedData` | **D**（→ **E**：至少"空文件"与"权重溢出"值得补成引擎用例） |
| 12 | 真实词表上跑同一套断言，且预期值不来自被测实现<br>`english.pilot.real_lexicon_candidates_commit_and_protection` | `recovery.pilot.real_lexicon_reference_compared` + 三条拒绝/控制 | 集成（另有独立参考实现与 CMake/`sha256sum` 双路核对） | A |
| 13 | 生成产物与锁定清单逐项对齐<br>`english.pilot.output_matches_the_pinned_manifest` | `recovery.pilot.output_matches_the_pinned_manifest` + 4 条 `recovery.pilot_sha256.*` | 数据 + 辅助 | A |
| 14 | P2.6 拼写建议：门槛表、四类编辑、`spell` 标签列、预算与长度窗口、三形状渲染、退格重算、抑制先于成员判定<br>`english.completion.spelling_suggestions_and_suppressions`（1 条） | `recovery.spelling.suggestions_and_suppressions`、`…commit_and_selection`、`…negative_lexicon_unavailable` + 2 条控制 | 集成（另有核心 case 32–38） | A |
| 14b | 三态 `empty-input` / `unsupported-byte` / `non-ascii-input`：真实按键进不了组合区 | 引擎侧只能断言其**为 0**（不可达） | 核心 case 36 门槛表 | **D** |
| 14c | `SpellingEdit` 编辑**种类**在 C API 不可见（`RimeCandidate` 只有 text/comment） | 引擎侧断言文本/次序/预算/标签列，种类由反向一次编辑参考实现与文档表断言 | 核心 `TestSpellingFourEdits` | **D** |
| 15 | **开关关闭等于没有本阶段**（夹具 README §7 那条声明） | 旧工程：无对照，只由**另一份词表 + 未命名该键的 schema** 间接承担；<br>现工程：`recovery.spelling.feature_toggle_ab`（同一份 16 行词表、同一串键入、两套受控 schema 只差开关，并读回**已部署**配置值）+ `…suite_can_fail_on_toggle_comparison` | 集成 | A（本轮补的直接证据；旧的间接证据同时保留为回归） |
| 15b | 高亮一行后按 Return 提交的是**被高亮那一行**（上游 `Composition::GetCommitText()` 读 `selected_index`） | **无任何注册用例断言过这一点**；本轮实测发现并记入 §3 批次 7 披露 | 无 | **E**（应在提交边界组补一条，与"空格时高亮不等于接受"并列） |
| 16 | pilot-spelling 引擎侧 A/B 成本试验<br>`english.pilot.spelling_suggestion_cost_a_b_on_pilot_data` | **不注册、不实现** | — | **C**（注册先于实现，从未进入任何一次绿色扫描，不属于事故前绿色基线，故不迁入正式注册） |
| 16b | 拼写成本试验的核心那一半 | `english.core.spelling_cost_trial_on_pilot_lexicon`（根工程，真实词表在位才注册） | 核心 | D（引擎那一半仍是 C） |
| 17 | 重复词条的行号归因不确定（`english_lexicon.cc:203-216` 在 `std::sort` 之后相邻比对） | 用例按夹具事实断言"行号 ∈ 重复对所处的两行"并打印实测值 | 集成 | A（测试侧处理）；**产品侧未决**，改成确定性归因属产品改动，等授权 |
| 18 | `docs/testing/rime-english-completion.md` 为每个 mode 写一节判定口径 | §6–§6.4 有 completion / case / token / pilot / space；**spelling 与 spelling-toggle 没有对应小节**，判据只在 `tests/fixtures/english_spelling/README.md` | 文档 | **E**（补 §6.5/§6.6） |

**B 类：空。** 旧工程 13 条常规注册 + 3 条 pilot 注册中，除第 16 行那条"注册先于实现"
的 pilot-spelling（C 类）以外，**每一条都有一个或多个当前具名用例对应**，且
`ctest --show-only=json-v1` 导出的清单与 `CMakeLists.txt` 里的注册逐名双向一致
（由正式入口在每次运行时机器核对，见 §4）。所以"恢复"这一件事到这里可以判定完成；
**但"英文能力齐全"没有完成**，见 §1B。

## 1B. AGENTS.md §1 的英文能力清单（产品视角，不是恢复视角）

这张表存在的唯一理由：矩阵 §1A 全绿只说明**测试能力从事故中恢复了**，
不说明产品能力已经齐备。两者不能混为一谈。

| AGENTS.md §1 条目 | 当前状态 | 证据所在 |
| --- | --- | --- |
| 拼写纠错 | **部分实现**：一次编辑（删/插/换/相邻换位）建议器，长度窗口 4..32，预算 ≤3，需 schema 显式 `enable_suggestions`，且**只建议不替换**。两次以上编辑、概率排序、词频纠错均未实现 | `engine/languages/en/spelling_suggester.{h,cc}`；§5.2 的 7 条 `recovery.spelling.*` |
| 词汇选择 / 词频 / 上下文学习 | **未实现**。pilot 权重是跨词典佐证的 2..12 档位，`weights_are_corpus_frequency: false`；无任何学习存储 | `docs/compliance/english-pilot-data.md` |
| 形态变化（复数、`-s/-ed/-ing`、所有格、缩合） | **未实现**。`don't`/`co-operate` 这类带分隔符词形当前是**拒绝纠正**，不是形态分析 | 夹具 §4 的 delimited 行 |
| 连写输入自动分段 | **未实现**。`helloworld` 仍是一个整串原文 | §1 表"空格与连续多词"行的范围声明 |
| 缩写展开 | **未实现** | — |
| 跨应用一致性 | **未实现**（各宿主进程各自持有引擎实例，无跨进程一致性层） | — |
| 非 ASCII / Unicode 输入 | **未验证**（引擎侧不可达，见 §1A 第 9b、14b 行） | 核心单测 |

## 2. 与新工程有关的两个事实

1. `tools/rime_english_completion/CMakeLists.txt`（mtime 2026-10-19 16:40）在 main.cc 已为
   0 字节之后被改过，其中 `pilot-spelling` 的注册（第 429-437 行）**先于实现存在**。
   因此旧工程无法构建成功。**该文件与那份 0 字节的 `main.cc` 已于批次 7 删除**
   （提交 `build: promote modular English integration test entrypoint`）；删除前确认
   没有任何 `add_subdirectory`、脚本或构建预设引用该目录。目录本身保留，
   里面只剩一个指向新正式入口的 `README.md`。本条其余部分按原样留存，是当时的事实。
2. 新工程复用存活的 `tests/english_completion/assert_exit.cmake`（精确退出码 +
   marker 策略），未新增第二种判定口径；也复用存活的
   `tests/fixtures/english_completion/`，夹具只读。

## 3. 顺序与已完成批次（每步一个可编译、可判定的增量）

已完成：批次 1 = 公共框架 + `completion`（6 条）；批次 2a = `case-preservation`（8 条）；
批次 2b = `token-protection`（10 条）；批次 3a = `space-commit`（12 条）；
批次 3b = 四类词表**加载**失败 + 该组可失败性控制（17 条）；
批次 4 = `pilot-lexicon` 真实词表 + 独立参考实现 + 数据身份三Kind拒绝（开关关闭仍 17 条，
开启 22 条）。
批次 5 = **只换测试工具的摘要后端**：删除 `pilot_sha256.cc` 的自实现 SHA-256，改为 Windows
CNG，并给它单独建一个不启动引擎的可执行文件与 3/4 条用例（开关关闭 20 条，开启 26 条）。
**产品、引擎、夹具、清单、判据一律未改动。**
批次 6 = **恢复 `spelling` 集成覆盖**：新增 `spelling_tests.{h,cc}`（门槛表 + 候选正确性 +
反向一次编辑参考实现）、`spelling_commit_tests.{h,cc}`（提交边界与双 Session）、
`spelling_negative_tests.{h,cc}`（`lexicon-unavailable` + 两条可失败性控制），`main.cc` 只加
派发，`recovery_test_support` 只加拼写夹具的暂存计划与两份派生文件的生成/校验，CMake 只加
源文件与 5 条具名用例（开关关闭 **25** 条，开启 **31** 条）。**产品、引擎、夹具、清单、判据
同样一律未改动；本轮不依赖 pilot 词表，也未运行任何拼写性能试验。**

待做：其余加载状态（空文件、空行、缺字段、多字段、权重非法/溢出、词太长）→
最后才是 `pilot-spelling`（新增，非恢复）。
`spelling` 已在批次 6 恢复；`pilot-spelling` 仍是"原本就未完成"，批次 6 **未实现、未注册**，
也没有为它建空壳。

批次 7 = **整体覆盖核对与正式测试入口迁移**（本轮）。三件事：
① 补上 spelling 开关的**直接对照**用例（`recovery.spelling.feature_toggle_ab` + 其控制条，
提交 `test: verify explicit spelling feature toggle behavior`）；
② 新增正式入口 `tools/test_english_integration.ps1`，把参数与工具检查 → 配置 → 构建 →
导出实际测试清单 → CTest → 真实退出码走成一条不可半途的策略，并让它**机器核对**
源码注册与生成清单一致；
③ 退役旧入口（删 0 字节 `main.cc` 与失效的旧 `CMakeLists.txt`，目录留指针 README），
把当前有效文档与脚本引用改到新入口。
**产品、引擎、判据文档、清单一律未改**；夹具目录只有**注释文本**变更（三个
`default.yaml` 的"Consumed by"行与三份 README 的消费方/生成物说明），YAML 语义、
`config_version`、schema 内容与全部词表**逐字节未动**。
开关对照**不使用 pilot 数据，不测性能**。数量指标：pilot 关闭 **27** 条（批次 6 后 25），
pilot 开启 **33** 条（批次 6 后 31）。

### 批次 7 的几处披露（本轮；置于各批次披露之首便于先读最新，批次 2a–6 的披露在其后，按批次顺序排列）

1. **我一开始把"高亮不等于接受"错误地推广到了 Return 上**。开关对照的正向用例里我加了一段
   "在开启侧高亮 index 1（`hello`）后按 Return，应当仍提交原文 `helo`"，实测
   `ASSERT FAIL … got "hello"`（临时驱动轮次的日志
   `build/english-recovery/toggle-ctest-spelling-r1.log`）。回上游核对：`Context::Commit()` →
   `ConcreteEngine::OnCommit()` → `Context::GetCommitText()` →
   `Composition::GetCommitText()` 取的是 `Segment::GetSelectedCandidate()`，
   而 `Context::Highlight()` 正是设 `selected_index` 的那个调用
   （`src/rime/context.cc:19-32`、`:132-137`，`src/rime/composition.cc:102-109`，
   `src/rime/engine.cc:251-257`）。所以**这是上游语义，不是缺陷**：
   "高亮不等于接受"讲的是**空格**走自有 `epocanva_english_space` case A 时提交原文，
   与 Return 不是同一条路径。处理方式是把这段**从开关对照里删掉**（它测的是提交边界，
   不是开关），并在 `spelling_toggle_tests.{h,cc}` 与夹具 README §7 写明这条边界，
   同时把它记为 §1A 第 **15b** 行的 **E 类缺口**（至今没有任何注册用例断言过它）。
   **没有**为了让绿色通过而放宽任何断言：那 1253 条里其余 1252 条都是先过的。
2. **正式入口自己有两处缺陷，都是实测暴露的**：
   ① 我把 `-Filter` 直接接到了"清单一致性核对"上，于是过滤运行必然报
   "注册的用例没出现在生成清单里"。改为**永远**先导出完整清单做一致性核对，
   再单独导出过滤选择集并要求非空、且每个名字都在完整清单里
   （`build/english-recovery/test/integration-logs/list-all-*.log` 与
   `list-filtered-*.log` 是两份日志）。
   ② PowerShell 侧两个真坑：`function f { return @() }` 返回空数组会被摊平成 `$null`，
   于是 `$selection.Count` 在 `Set-StrictMode -Version Latest` 下抛
   `PropertyNotFoundStrict`；以及 `[Parameter(Mandatory)][string]$Regex` **拒绝空字符串**，
   让"无过滤"这条路自己炸掉。两处都实测复现后修掉（外层一律 `@(...)` 包裹；
   参数改 `[AllowEmptyString()]`）。
3. **一次构建失败被我用来当证据链的一环，而不是当事故**：为了实证"构建失败不会去跑旧
   exe"，我在本轮新建的 `spelling_toggle_tests.cc` 里临时加了一行
   `static_assert(false, …)`，跑正式入口得到 `FAILED [build] exit=5`，
   该 `-LogTag` 下只有 `configure-*.log` 与 `build-*.log`、**没有 ctest 日志**，
   而 `english-engine/rime_english_recovery.exe` 的 mtime 仍是上一轮那个
   （`build/english-recovery/test/integration-logs/{configure,build}-buildfail.log`）。
   随后撤销该标记行并用 `git diff --exit-code` 证明文件与工作树一致。
4. **旧 CMake 的注册数在我自己的记忆里是 11 条，实际是 13 条常规 + 3 条 pilot**。
   旧文件里 `# The default 11 cases above` 是它自己的过期注释。本轮删除前逐行核过
   `epocanva_add_case` / `add_test` 的出现位置，§1A 的对齐以**读到的文本**为准，
   不以旧注释的自述为准（AGENTS.md §6）。
5. **`tests/english_core/main.cc` 里有一处过度声明被我改掉**：`--pilot-bench` 的注释原本写
   "核心那一半 + 引擎那一半由 `tools/rime_english_completion --mode pilot-spelling` 测量，
   两者分别报告"。引擎那一半从未存在（C 类），所以那句话把一件没做过的事写成了做过。
   现改为"只有核心这一半存在，引擎那一半从未实现也从未在本工程注册"。
   同文件还引用过一份**不存在的**报告 `docs/reports/P2.6-spelling-suggestions.md`
   （`tests/english_core/CMakeLists.txt` 里），改指向本文。
6. **根工程实际是 8 条而不是文档里写的 7 条**，英文核心是 4 条而不是 3 条，
   核心单测内部是 1566 条断言而不是文档里的 1196 条。三者都是本轮实跑读数
   （`build/regressions/root-ctest-r1.log`，直跑 `--unit` 打印
   `1566 checked, 0 failed`）。文档按实测改正。
7. **`ctest --version` 一类的工具自检不能用 `| Select-Object -First 1`**：提前结束原生命令的
   管道会让 `$LASTEXITCODE` 变成 `-1`，我第一次的运行因此把"CTest 正常"判成失败。
   改为整体消费输出之后再取第一行。
8. **工具发现必须按"这台机器上哪个实例真的带 CMake"筛，而不是按"有没有 VS"**：本机
   VS2022 Community 有 DevShell 但没有内置 CMake，Professional 才有；只按 DevShell 筛会
   选到 Community 然后失败。正式入口现在要求实例同时带 DevShell + `cmake.exe` + `ctest.exe`，
   都不满足时报出实例清单并以 3 退出（不安装、不改 PATH）。**这次失败本身被完整保留**：
   它是"缺工具清楚报错"第一条现成的证据。
9. Git：未 push、未 tag、未 amend、未 `reset --hard`、未 `clean`；两次提交各自只暂存本轮
   路径，提交沿用命令级空 `hooksPath`（`-c core.hooksPath=build/empty-hooks`，指向一个
   确实为空的目录，因此 `.git/hooks/post-commit` 之类未知 hook 不会被执行），
   未改任何全局或仓库配置。

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

### 批次 3b 的几处披露（同样全部是测试或工具脚本自身的错误，产品行为未改）

1. 四类加载用例首轮各有且仅有 **1 条**失败：我把 `Return` 也计入"每个探测键都应到达翻译器
   一次"的键数（要求 `queries >= 7`，实测 6）。Return 在已提交的空组合上不发起查询，因此
   计数只应含字母键——变量随之改名 `probe_letters` 并注明这一点。**没有任何加载断言失败**：
   `file-not-found/0`、`duplicate-word/3`、`illegal-word/3`、`illegal-word-form/3` 首轮即全部命中。
2. 一次 `cmake --build` 因 `C2065`（改名未做完）失败，故**紧随其后**的那一轮 ctest 结果作废
   ——它跑的是旧二进制。重新构建通过后才再跑，日志另存为 `ctest-3b-newonly3.log`；作废那轮
   的 `ctest-3b-newonly.log`、`ctest-3b-newonly2.log` 原样保留。
3. 手工单跑起初用 PowerShell `Start-Process`，在本仓库含空格的路径上参数被拆断，五个模式一律
   exit 2（`FAIL arguments: unrecognised argument 'Kevin\OneDrive\…'`）。这是**脚本缺陷，不是
   测试结论**；原始日志保留（`evidence-3b-*.log`），改用 `&` 调用 + `$LASTEXITCODE` 后重跑，
   结果写入新的 `-r2` 日志并检查目标不存在才写。
4. 重复词条的行号归因不是确定值（见 §1 表末行的实现观察），因此该用例断言"行号属于夹具中
   持有该重复词的两行之一"并打印实测值；其余三类是顺序扫描，行号按精确值断言。

### 批次 4 的几处披露（同样全部是测试或工具自身的错误，产品行为未改）

1. **自实现的 SHA-256 头一版是错的，而且错在两处**：K 表尾部凭记忆手写（伪造值），压缩
   函数的 `t2` 误用了小 σ。改为从根的小数部分推导常数（`FractionTop32(sqrt(p))`、
   `FractionTop32(cbrt(p))`），并先用 FIPS 180-4 的 `""`、`abc`、56 字节三向量验过、再对
   真实产物求摘要与 `sha256sum` 相等之后，才让任何测试依赖它。教训按原样记在这里：**不要
   背常数**。
2. 数据身份因此有三个互不相干的来源：CMake `file(SHA256)`（配置期）、外部 `sha256sum`
   （命令行）、本工程自实现的 `pilot_sha256.cc`（运行期）。三者给出同一个
   `6e46587d…aae6b`。清单没有被改动去迎合任何产物，产物也没有被改动去迎合清单。
3. 独立参考实现 `pilot_reference.{h,cc}` 不 include 也不调用 `prefix_completer`、
   `english_case`、`english_token`、`english_lexicon`；它的排序/去重/截断另用 `awk` 对
   `co-`、`pro`、`world` 三组独立核对过，与引擎、与自身实现三方一致。
4. `DeployFixture()` 检查的编译产物名原先硬编码为 `epocanva_test_english.schema.yaml`，
   现改为**由本次运行实际暂存的 schema 文件名派生**。理由是判定强度：否则 pilot 运行可以
   靠"找到另一套夹具的编译文件"假装部署成功，54 行夹具那 17 条则不受影响（派生出的名字
   与原来相同）。
5. 两条"设计上 exit 22"的负向用例，其成功状态本身是非零码，因此"进程非零退出"绝不等于
   控制成立。两条模式都改为看护自己声明的断言：任一失败即返回 **18**，与 22（按设计拒绝）、
   14（引擎预期不符）互不混淆。这一点是实测的，不是推理的——用**未编译进身份预期**的那个
   构建目录跑同一模式，得到 `exit=18`、两条 `ASSERT FAIL` 指名"refused for an identity
   reason, named"与"the original pilot file is untouched"
   （`build/english-recovery/evidence-pilot-truncated-noexpectation-r1.log`）。
6. 身份拒绝的三个层次也各自实测：配置期 `FATAL_ERROR` 分别命中"字节数不符"（截断副本
   376,749 vs 502,333）、"文件名与清单不符"、"SHA-256 不符"（同长度、只改一个字母的副本）；
   运行期正向模式对同一份同长度副本给出 `22 / digest-mismatch` 且 `0 failed assertion(s)`
   —— 这与引擎报 `file-not-found`、与断言失败 14 都是**不同的种类**。
7. 落盘后编译错三处（`QueueEmpty` 里 `h.api` 应为 `h->api`，连带一个 `const` 未初始化报错；
   `CheckMenu(run.last(), …)` 传的是 `KeyEvent` 而形参是 `StateSnapshot`），修完构建 0 error
   0 warning。更早的一版草稿引用了根本不存在的 `CountBars`/`type_probe`/`LastProbe`/
   `ev_keys_` 与 `Harness::run_root`，整份废弃重写而非原地打补丁。
8. `patch_support_pilot.py` 沿用批次 1 起的纪律：每个锚点先要求**恰好命中一次**，任何一处
   命中数不符即中止且不写盘；写盘前另存 `.bak`。首轮因锚点与实际枚举末尾不符而安全中止，
   重新读取真实文本后才通过（6 处头文件替换 + 11 处实现替换）。
9. **数据不进 Git**：产物仍只在 `build/english-pilot/`（被 `.gitignore` 的 `/build/` 覆盖），
   本轮没有新增或修改任何夹具，没有改动产品解析器，没有下载任何东西，配置/构建/测试全程
   无网络步骤。
10. 范围声明：pilot 的 28 行查询集全部是 ASCII（终局 `nonascii=0`），所以它提供的是
    **数据规模**证据而不是 Unicode 输入证据；按键仍经 `RimeApi::process_key()`，不是 Windows
    TSF 宿主层验证；逐键耗时分布只打印、不判定，也不做任何拼写 A/B 或索引改写。
11. `pilot-spelling` 仍是"原本就未完成"，本轮**未实现、未注册**；`spelling` 仍未恢复。

### 批次 5 的几处披露（测试工具后端的替换，产品行为未改）

1. **删除了自研 SHA-256**：`pilot_sha256.cc` 里的手写轮函数、`Ch/Maj/σ/Σ`、从素数根推导常数的
   `BuildConstants()` 全部移除，不留备用自研实现，也不保留常量表。对外只留一个很小的文件摘要
   接口：`Sha256FileDigest()` 返回 `{失败种类, 摘要十六进制, 命名细节}`，`Sha256HexOfFile()`
   保持原签名与原语义（失败即空串），因此 `pilot_tests.cc` 只在"为什么失败"这一处变了。
2. **失败种类不再合并**。批次 4 只有一个 `digest-unreadable`；现在 CNG 侧的失败按
   `digest-open-failed / digest-read-failed / digest-hash-failed` 命名，`digest-mismatch`
   只用于"读到了、算出了、值不同"。截断负向用例白名单仍只接受 `size-mismatch /
   digest-mismatch / row-count-mismatch`，所以打开失败或哈希 API 失败**不可能**被它当成成功。
3. **出错不产出摘要**：`CreateFileW` 失败、`ReadFile` 失败、任何一个 `BCrypt*` 返回错误、或
   `BCryptGetProperty(HashDigestLength)` 不等于 32 字节，`hex` 一律为空——不会是全零、截断或
   半成品，因此不可能与清单里的 64 位十六进制期望相等。
4. **向量期望值来自被测实现之外**：15 个夹具的字节由程序按两条公式自己生成并写进本次运行的
   scratch 目录（`tests/fixtures/` 与 `build/english-pilot/` 全程只读），而期望值由
   python hashlib、GNU coreutils `sha256sum`、PowerShell `Get-FileHash` **三个互不相同的实现**
   算出且彼此一致；本轮没有为向量测试再写一套算法。附带一条诚实记录：我凭记忆写出的 56 字节
   样例句摘要尾部是**错的**（记成 `…6a7472a3`，实测 `…d419db06c1`），外部工具当场把它纠正了——
   这正是 AGENTS.md §6 "不要凭记忆"的又一例，与批次 4 的教训同类。
5. **负向结果各自实测**：同长度只改一字节的**独立副本**（第 1000 字节 `'1'→'0'`，长度仍
   502,333）在正向模式上得 `22 / digest-mismatch` 且 `0 failed assertion(s)`，CNG 算出的
   `4024e5ac…` 与 `sha256sum` 对同一副本的结果一致；缺失路径与目录都报 `open-failed`
   （win32 error 2 / 5）且摘要为空；截断副本（65,537 的前 65,536 字节）报的是**另一个文件的合法
   摘要**，即"读通了但身份不符"，不是 I/O 失败。真实词表在改动前后都仍是
   `6e46587d…aae6b`，未被任何一步改写。
6. **一处没能按预期落到摘要层的结果，如实记录**：我另建了一份"存在但拒绝读"的副本（对
   build/ 内的独立副本自身施加 `icacls /deny`，跑完立即 `/remove:d` 复原并核对摘要=原值），
   想让正向模式报 `digest-open-failed`；实际先被 `RunHarness` 的夹具暂存拦下，得
   `fixture.copy … Access is denied`、**退出 6**，根本没走到摘要。结论是"打开失败被当成摘要
   不匹配"在这条路径上不可能发生（它连 22 都到不了），但 `digest-open-failed` 这一分类目前
   只有辅助层用例证明，引擎层没有触发过。
7. **`kReadFailed` / `kHashFailed` 没有被运行时触发**：构造它们需要故障注入，本轮没有加注入
   钩子，也没有为了让它们变绿而写第二套实现。二者只在类型、命名和"必然空摘要"这三点上被断言
   与其他种类可区分；要真正实证，需要一个测试专用的读取/哈希失败钩子，属未做项。
8. **bcrypt 的范围是查证过的，不是声明的**：`dumpbin /IMPORTS` 显示 `bcrypt.dll` 只出现在
   `rime_english_recovery.exe`、`rime_english_recovery_baseline.exe`、
   `rime_english_recovery_sha256.exe` 三个测试可执行文件里；`rime.dll`（english 与 baseline
   两侧）仍只导入 `KERNEL32.dll / USER32.dll`，`engine/` 下 `grep -i bcrypt` 零命中。摘要辅助的
   新可执行文件不导入 `rime.dll`，其输出目录里也没有 `rime.dll`。
9. **不新增任何安全声明**：本轮把"我们自己实现的哈希"换成"操作系统提供的哈希"，改变的是实现
   归属，身份契约不变——依旧是"该字节序列与该清单记录一致"。**它不是**对来源作者的签名验证，
   也没有引入任何密钥、信任根或网络步骤（`sha256sum`/`Get-FileHash`/python 只在构建期用于**算
   期望值**，运行时程序不启动任何外部 shell）。配置期继续用 CMake `file(SHA256)`，未改 TSV、
   未改清单、未降低校验强度。
10. **跨平台不宣称**：`pilot_sha256.cc` 现在直接依赖 Windows CNG，因此该辅助是 Windows-only；
    本工程本来也只在 MSVC/x64 上配置（`CMakeLists.txt` 第 47-50 行的 `FATAL_ERROR`）。

### 批次 6 的几处披露（同样全部是测试或工具自身的错误，产品行为未改）

1. **我自己写的注释列拼接函数吞掉了第一个分隔符**，三处（`spelling_tests.cc` 的
   `CommentColumnShape`、`spelling_commit_tests.cc` 与 `spelling_negative_tests.cc` 的 `Column`）
   都用"输出还空着就不加竖线"的规则，而候选 0 的注释**恒为空串**，于是 4 项菜单的
   `|spell|spell|spell` 被拼成 `spell|spell|spell`、2 项补全菜单的 `|` 被拼成空串。首轮 gates
   用例报 **15 条失败，全部是注释列格式**，**没有任何门槛、候选、排序或计数断言失败**——
   这一点是从日志里逐条读出来的，不是推断的。改为统一使用 Harness 早已导出的
   `JoinComments()`（按索引加竖线），把三个本地实现删掉并在注释里写下这条理由。
   失败日志 `spelling-ctest-off-gates-r1.log`（`--output-verbose` 这个 ctest 不认的参数）与
   `-r2.log`（上述拼接缺陷）原样保留，修好后的运行写在 `-r3.log`，最终二进制再单跑一次写在
   `spelling-ctest-off-final-gates-r1.log`，三次数字可比且后者为验收依据。
2. **`ctest -R` 在一条用例都没匹配上时返回 0**（`No tests were found!!!` + `EXIT_CODE=0`）。
   我第一次给提交组传的是 `--mode` 名而不是用例名，那一份"exit 0"实际什么都没跑。发现后按用例名
   重跑并**核对 `1/1 Test #19 … Passed` 这一行**，日志另存 `spelling-ctest-off-commit2-r1.log`；
   什么都没跑的那份 `spelling-ctest-off-commit-r1.log` 保留。教训按原样记：**判定要看跑了几条，
   不能只看退出码**。
3. **不复制事故前脚本的逐帧状态表**，理由是那份文本自己就不自洽：
   `patch_spell_mode.py` 第 396-397 行 `{"hello", "TTT" "w", …}` 与 `{"HELLO", "TTT" "w", …}`
   两行各有 4 个状态码，而被键入的文本是 5 个字符，与该脚本自己"每字符一码，否则以 2 退出"的
   检查冲突；而 `build/p26-verify/mode-spelling.log` 又记录该模式当初 3057 条断言 0 失败，说明
   落地文本与这份补丁草稿并不一致（目录里另有 `fix_literals.py`，我**没有执行任何**事故期脚本，
   也无法从文本判定是哪一步改的）。因此本轮的逐帧预期由 `spelling_tests.cc` 里的**反向一次编辑
   参考实现**按 `get_input()` 观察到的实际组合现算，另外保留一列"文档结论"（`want_state`）与
   README §4 的 `(权重,编辑)` 字面量表互相核对。
4. **派生夹具的锚点纪律**：`epocanva_spell_broken_english.schema.yaml`（3295 字节）与三行重复
   词表（23 字节）由程序在暂存期生成。锚点是**带两格缩进的 YAML 键加换行**，替换前先要求恰好
   命中一次，按**从后往前**的顺序替换（新 id 比旧 id 长 7 字节），替换后再核验"旧 id 行消失 /
   新 id 行恰一次 / 坏词表路径恰一次 / 好的带引号路径为 0 次"，任一条不满足就以阶段名
   `spelling.broken-schema.rewrite` 返回 5，绝不静默部署一份"文件名说 A、内容说 B"的 schema。
   写盘顺序是"内存里构造并校验文本 → 目标必须不存在 → 写后回读比对"。夹具头部注释
   `# schema_id: epocanva_spell_english` **故意不改**——它正是那个松锚点陷阱的活样本，而
   `SelectAndConfirmSchema()` 读回的 `get_status().schema_id` 才是引擎真正采用的 id。
   `tests/fixtures/` 全程只读：`git status tests/` 为空，两个目录树未被任何一步写入。
5. **每个用例独立运行目录、独立进程**，且**不依赖 pilot 词表**：5 条 `recovery.spelling.*` 都跑
   手工夹具的 16 行受控词表（`loaded_entry_count == 16` 与状态名 `ok` 是被断言的事实）。
   `pilot-spelling` 未注册、未实现、未运行任何性能试验。
6. 编译期错一处（`Checker::CheckStr` 被传了 4 个参数）首轮构建即暴露并修掉；最终
   `spelling-build-off-r3.log` 与 `spelling-build-on-r1.log` 均 **0 error 0 warning**。
7. **未发现产品缺陷，`engine/` 一个文件都没改**：门槛顺序、四类编辑、去重、同分排序、预算截断、
   三种大小写渲染、`spell` 标签列、A/B/C 空格规则与夹具文档完全一致。三态
   （`empty-input`、`unsupported-byte`、`non-ascii-input`）在引擎侧**不可达**（空组合不发起查询；
   `;` 之类字节不在 `speller/alphabet` 里，真实按键进不了组合区），本轮如实断言它们为 0 并写明
   理由，没有伪造触发，也没有把它们算进"已覆盖"。
8. Git：未 push、未 tag、未 amend、未 `reset --hard`、未 `clean`；只暂存本轮 10 个文件路径，
   提交沿用命令级空 `hooksPath`，未执行任何未知 hook。

## 4. 正式测试入口与批次 7 的实测结果

### 4.1 入口

`tools/test_english_integration.ps1` 是唯一受支持的英文集成测试运行方式，驱动
`tools/rime_english_recovery/`。三个必填参数：`-EnginePrefix`、`-BaselinePrefix`、
`-BuildDir`；可选 `-PilotLexicon`（默认不给 → 不注册 pilot 用例）、`-PilotOutputDir`、
`-Filter`、`-LogTag`、`-LogDir`、`-RepoRoot`、`-SourceDir`、`-Generator`、`-Platform`、
`-VsInstallPath`。仓库根由脚本自身位置推导，**没有任何个人机器绝对路径被写进文件**。

阶段与退出码：`0` 通过；`2` 调用被拒（含日志已存在）；`3` 工具/产物缺失；`4` 配置失败；
`5` 构建失败；`6` 清单与注册不一致或选择为空；`7` 拿不到可判定的计数；`8+` 原样返回 CTest 的码。
它不下载、不安装、不改 PATH、不清理、不重建依赖，`build/` 之外的东西一律不删。

三道反空转闸门：每次 CTest 调用都带 `--no-tests=error`；带 `-Filter` 时先确认匹配清单
非空且每个名字都在完整导出里；判定同时要求 CTest 自己汇总行的"跑了几条"等于导出的
选择集大小——**日志里出现 `RESULT PASS` 从来不是判据**。

### 4.2 两套配置的实际清单与结果（2026-10-22 实测）

| 配置 | `-BuildDir` | 源码注册 | CTest 生成 | 跑出 | 失败 | 脚本退出 | 证据 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| pilot 关闭 | `build/english-recovery/test` | 27 无条件 + 6 pilot-only | **27** | 27 | 0 | **0** | 最终一轮：`build/english-recovery/test/integration-logs/{configure,build,list-all,ctest-all}-FINAL-off.log`（文档改动后的同一套配置另有一轮 `-afterdocs-off`，结果相同）|
| pilot 开启 | `build/english-pilot-recovery/test` | 同上 | **33** | 33 | 0 | **0** | 最终一轮：`build/english-pilot-recovery/test/integration-logs/*-FINAL-on.log`（`-afterdocs-on` 同结果）|

pilot 开启那一行同时打印配置期身份结论：
`pilot identity : OK sha256 6e46587d…aae6b 502333 bytes 42356 rows source
alt12dicts/2of12full.txt @ 7bbe293425d48a1210bf17aaf156b28ade5c2de1`。
两套构建均 **0 warning 0 error**（构建日志逐行匹配 `: (warning|error)` 计数为 0）。

开关对照那条在最终二进制上的读数：`recovery.spelling.feature_toggle_ab`
**1253 条断言 0 失败**；其控制条 `recovery.spelling.suite_can_fail_on_toggle_comparison`
**36 条断言 1 失败**、退出码 14、marker 未出现，失败项就是那句故意写错的
"切换开关什么都没改变"。派生那一侧 `toggle.derived epocanva_spell_off_english.schema.yaml
(3286 bytes, 74 lines, 2 lines apart from the source)`，原件是 3281 字节 / 74 行，
差的 5 字节 = 身份 4 + `true`→`false` 的 1。配置回读两侧打印：
`enable_suggestions=read(1)` 与 `enable_suggestions=read(0)`，
其余 `max_suggestions=3 max_completions=7 page_size=8
lexicon_path=lexicon/epocanva_spell_lexicon.tsv` 两侧逐字相同。
`api.balance` 末行含 `config 2/2 -- paired`。

### 4.3 失败控制（本轮实测，不是推理）

| 要验证的纪律 | 怎么触发 | 实际结果 |
| --- | --- | --- |
| 参数错误返回非零 | 不给任何参数 | `FAILED [arguments] exit=2`，打印 usage |
| 前缀指向不存在的东西 | `-EnginePrefix build/does-not-exist` | `exit=2` |
| pilot 数据缺失**不静默跳过** | `-PilotLexicon <不存在的路径>` | `exit=2`，消息明说"pilot 用例不会被静默丢弃" |
| pilot 路径为相对 | `-PilotLexicon build/english-pilot/…tsv` | `exit=2` |
| pilot 身份不符 | 对真实清单做**同长度改一字节**的独立副本（第 1001 字节 `xor 0x01`，长度仍 502,333，副本 sha `4024e5ac…`），指给它 | 配置期 `pilot data identity FAILED on SHA-256`，`FAILED [configure] exit=4`；该 `-LogTag` 下**只有** configure 日志 |
| 零匹配不能判绿 | `-Filter recovery.nosuchgroup.` | `FAILED [list] exit=6`：matched none of the 27 generated case(s)，CTest 未被调用 |
| 构建失败阻止后续测试 | 在本轮新增的 `spelling_toggle_tests.cc` 里临时插一行 `static_assert(false,…)` | `FAILED [build] exit=5`；无 ctest 日志；旧 exe mtime 未变。随后撤销并用 `git diff --exit-code` 证明复原 |
| 日志不覆盖 | 同一 `-LogTag` 跑第二次 | `FAILED [logs] exit=2`，指名拒绝覆盖的路径 |
| 缺工具清楚报错 | （非人为触发）机器上 VS2022 Community 有 DevShell 但无内置 CMake | `FAILED [tools] exit=3`，打印探针命令与实例清单，不安装、不改 PATH；入口改为按"同时带 DevShell + cmake + ctest"筛选后选中 Professional |

### 4.4 本轮实际执行范围（明确记录，不含未跑项）

跑了：正式入口 pilot 关闭全套 27 条；正式入口 pilot 开启全套 33 条；`-Filter` 命中 2 条的
过滤运行；上表 9 项失败控制；根工程 `cmake --preset vs2022-x64-debug` 配置 + Debug 构建 +
8 条 ctest（4 基础设施 + 4 英文核心，核心单测直跑 `--unit` 报 `1566 checked, 0 failed`）；
`build/librime-baseline/replay` 2 条；`build/librime-baseline/smoke` 1 条；
`build/librime-component-probe/probe` 5 条。日志在 `build/regressions/*-final-r1.log`
（文档与注释定稿后又跑了一轮，`build/regressions/*-r1.log` 是夹具文本改动后的那一轮，
两轮数字相同）。

**没跑**：`tools/build_librime_baseline.ps1` 的任何模式（引擎与依赖一律复用现有产物，
未重建、未下载）；pilot 数据的生成脚本；任何性能/计时对比；任何 UI、任何其它语言。
`build/english-build.ps1` 的 `tool` 阶段被改为调用新入口，但该脚本整体**未在本轮执行**
（它是 git 忽略的临时驱动，改动只保证不再指向已删除的目录）。

### 4.5 事故恢复的结束条件

- §1A 的 **B 类为空**：旧工程 13 + 3 条注册里，除那条"注册先于实现"的
  `pilot-spelling`（C 类，本就不属于绿色基线）外，每条都有当前具名用例对应。
- 源码注册与 CTest 生成清单由入口在每次运行时机器核对，两套配置分别为 27/33 且无重名。
- 两套配置全绿，失败控制 9 项各自以预期的非零码退出，回归 16 条全绿。
- 旧入口已退役，当前有效文档与脚本引用全部指向新入口；历史报告未回改。

**结论：集成测试能力的事故恢复到此判定为完成。**
以下**不**因此视为完成，且都不属于恢复范围：§1B 里除"一次编辑建议"之外的英文能力、
§1A 中标 **D**（仅核心覆盖）与 **E**（待补注册）的每一行、以及第 17 行那条产品侧
行号归因未决项。


