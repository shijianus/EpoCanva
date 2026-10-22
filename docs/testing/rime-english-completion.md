# 英文当前词前缀补全的验证方法（P2.1）

本文说明 `engine/languages/en/`、`engine/rime_components/english/`、
`tests/english_core`、`tools/rime_english_recovery` 与
`tests/fixtures/english_completion/` 的**构建方法、运行方法、判定口径与失败注入方法**。

> **集成测试的入口已经变更**（事故后的模块化重建）。当前唯一受支持的运行方式是
> `tools/test_english_integration.ps1`，它驱动 `tools/rime_english_recovery/`；
> 旧的 `tools/rime_english_completion/` 已退役，目录内只留一个指向本文件的 README。
> 本文 §4 与 §5 按新入口书写；§6–§10 写的是**模式级判定口径**，那些口径跨迁移仍然有效，
> 但里面出现的 `english.completion.*` 是事故前的注册名，现名与逐条去向见
> `docs/reports/integration-harness-recovery-status.md` 的覆盖矩阵。

阶段结论见 `docs/reports/P2.1-english-prefix-completion.md`（前缀补全）与
`docs/reports/P2.2-english-case-preservation.md`（ASCII 大小写映射与原文保真）；
接入形态（编译进 `rime.dll`）的决策与限制见
`docs/adr/0004-compiled-in-rime-components.md`；
词表语法与候选期望的逐条推导见 `tests/fixtures/english_completion/README.md`。

---

## 1. 这套测试证明什么，不证明什么

**证明**（全部由真实运行时读数判定，不由"编译通过"或"源码里有这段"判定）：

1. `-Mode english-completion` 产出的 `rime.dll` 里存在 `epocanva_english` 模块，
   且它的 `initialize()` 被 librime 自己的装载机制调用过；
2. 该模块注册的 `epocanva_english_translator` 被真实引擎构造为实例；
3. 词表被读入并建成只读索引，查询结果作为**真实候选**出现在 `get_context().menu`；
4. 候选 0 恒为原文，其后按 weight 降序、同分按 ASCII 升序，且与原文相同的词条不重复；
5. 不选择直接提交 → 原文；`select_candidate()` 显式选中补全 → **提交完整单词**；
6. 退格重新查询、取消不残留提交、两个 Session 互不污染；
7. 词表缺失、词表非法、模块缺失这三种情况各有独立可观测签名。

**不证明**（每一项都是"未实现"，不得写成已通过）：

| 项 | 状态 |
| --- | --- |
| 下一词预测、词频/上下文学习、形态变化、自动分段、缩写展开 | **未实现**。`AGENTS.md` §1 的这些条目完全未触及 |
| 拼写纠错（P2.6，**已实现但默认关闭**） | **已实现为"一次编辑"建议器**：删除 / 插入 / 替换 / 相邻换位四种，长度窗口 4..32，预算 ≤3，与建议互斥于补全，`spell` 注释列是 C API 唯一能区分两者的东西。它是**建议不是替换**：默认提交仍是用户自己打的字节，只有显式选中才提交建议。它由 schema 的 `english/enable_suggestions` **显式开启**，未命名的 schema 行为与 P2.5 完全一致。**不是**编辑距离 ≥2 的纠错、不是语言模型、不打分概率；词表权重是夹具的手工档位 |
| ASCII 大小写映射（P2.2） | **已实现**：`lowercase` / `initial-capital` / `uppercase` 三种形状可补全并按原形状渲染；`mixed` 与含非字母的一律不补全。**不是** Unicode case folding，也不做句首自动大写或专名恢复 |
| 撇号与连字符（**仅** ASCII `'` 与 `-`） | **已实现为受限词形**（P2.3）：`[a-z]+`、`[a-z]+'[a-z]+`、`[a-z]+-[a-z]+`，至多一个分隔符、永不在首尾、两种分隔符不同现；查询前缀**可以**以一个分隔符结尾（`don'`、`co-`）。多分隔符（`rock'n'roll`）、Unicode 弯撇号、`–` 一律不支持，只保留原文 |
| 数字与 11 个技术符号 `@ . _ : / \ ? & = # % +` | **不补全**（P2.3 的保护策略）：这些键被 `speller/alphabet` 放行进组合区，再由词形层**整词**拒绝，判为 `protected-text`；纯字母串里混入数字则判 `not-alphabetic`。**不改写、不裁剪、不提取局部** |
| 非 ASCII 字节 | 核心层判 `non-ascii`；引擎侧**未验证**——不在 `speller/alphabet` 的键根本进不了组合区，真实按键路径不可达，只在核心单测里覆盖 |
| 词表规模与性能 | 手写 **54 行**（P2.3 起含 5 行带撇号/连字符词条）；**P2.4 增加 `pilot-lexicon` 模式**，在 **42,356 行**真实词典清单上跑同一套断言（来源/授权/转换政策见 `docs/compliance/english-pilot-data.md`）。核心 `--bench` 与 pilot 的 `scale.*` 数字都**不可**外推为生产性能，也不构成与任何输入法的比较 |
| 空格与连续多词输入（P2.5） | **已实现**：三条规则（有组合→提交原文 + 一个 U+0020；已显式选择→提交该词一次 + 一个空格；空组合→只提交一个空格），词与词之间重新计算大小写/保护/候选。**不是**连写切分：`helloworld` 仍是一个整串原文；也**不**跨已提交文本删除或替换，空组合下的 BackSpace 仍按上游语义交给宿主处理 |
| 词典 / 用户词库 / 学习存储 | 本组件**不使用**（不派生 `rime::Memory`，所以**它自己**没有词典与学习挂钩）。**这不等于引擎不写盘**：`select_schema()` 仍会写 `user.yaml`，见 §9 |
| Unicode / 字素簇 / 屏幕列换算 | **未验证** |
| TSF、Android、安装器 | **未实现** |

## 2. 前置条件

- 已完成一次 `-Mode baseline`（P1.2 产物在位）：四个依赖装在
  `build/librime-baseline/prefix/`，Boost 头树已解压。
  **`english-completion` 不重建、不下载任何依赖**，它只读复用同一套 `prefix`
  并在脚本里重新断言其来源。
- 工具链：VS2022 Professional、生成器 `Visual Studio 17 2022`、`-A x64`、
  其自带 CMake/CTest `3.31.6-msvc6`。本机 CMake 不在常规 PATH，
  须先进入开发者环境（`docs/build/bootstrap.md`）。
- 组件钩子用 `cmake_language(DEFER …)`，需要 CMake ≥ 3.19。

## 3. 三层测试分别跑在哪

| 层 | 工程 | 构建空间 | CRT |
| --- | --- | --- | --- |
| 语言核心单测 | 根工程 `tests/english_core` | `build/vs2022-x64-debug` | Debug / 动态（核心不含 rime，故可独立编） |
| 引擎与组件 | `tools/build_librime_baseline.ps1 -Mode english-completion` | `build/librime-english/` | Release / `/MT` |
| 集成测试 | `tools/rime_english_recovery`，经 `tools/test_english_integration.ps1` | `build/english-recovery/test` | Release / `/MT` |
| 集成测试·真实词表（P2.4） | 同一份模块化源码，`--mode pilot-lexicon` | `build/english-pilot-recovery/test` | Release / `/MT` |

核心层**不含任何 Rime 或平台头文件**，所以它能被根工程的 Debug 工程直接编译并单测；
同一份源文件列表由 `engine/languages/en/CMakeLists.txt` 同时供给引擎构建，
两边测/跑的不是两份拷贝。

## 4. 命令

### 4.1 核心单测（根工程）

```sh
cmake --preset vs2022-x64-debug
cmake --build build/vs2022-x64-debug --config Debug
ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error
```

手工运行与两个附加模式：

```sh
build/vs2022-x64-debug/tests/english_core/Debug/epocanva_english_core_tests.exe --unit
#   -> exit 0，末行 "RESULT PASS"，另打印 "213 checked, 0 failed"
… --forcefail     # 故意写错一条断言，必须 exit 1；若 harness 吞掉假断言则 exit 3
… --bench         # 小规模计时样本，仅打印，不作判定
```

### 4.2 英文引擎

```sh
tools/build_librime_baseline.ps1 -Mode english-completion -Fresh -Verbose_Build
```

断言链：`EPOCANVA_*` / `CMAKE_PROJECT_rime_INCLUDE` 逐字回读、
`BUILD_MERGED_PLUGINS=ON`、`ENABLE_EXTERNAL_PLUGINS=OFF`、
configure 输出里**恰好一行** `EpoCanva P2.1 ATTACHED:`、
四个组件 `.obj` 按名字存在、逐件 `dumpbin /directives` 的 `MT_StaticRelease`、
DLL 哈希 ≠ baseline、组件字符串命中、导出表差集为空、
baseline 的 rime CMakeCache 不含任何 `^EPOCANVA_` 条目。

增量重跑（**不带** `-Fresh` / `-Verbose_Build`）也必须 exit 0；
这是 P1.4 遗留误报的回归点，判据见 §4.5。

### 4.3 集成测试：正式入口

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 `
  -EnginePrefix   build/librime-english/rime/install `
  -BaselinePrefix build/librime-baseline/rime/install `
  -BuildDir       build/english-recovery/test `
  -LogTag         <本轮后缀>
```

三个参数都是**必填**且按调用者的当前目录解析（含空格路径无需任何引号技巧：脚本用
`& $exe @数组` 逐项传参，不走 shell 插值）。脚本按
参数与工具检查 → 配置 → 构建 → 导出实际测试清单 → CTest → 真实退出码 一条线走完，
每步失败立即停止，退出码区分阶段：

| 退出 | 含义 |
| --- | --- |
| 0 | 全部通过 |
| 2 | 调用被拒：必填参数缺失/畸形/指向不存在的东西，或该 `-LogTag` 的日志已存在 |
| 3 | 工具或产物缺失（打印探针命令；**不安装、不改 PATH、不修工具链**） |
| 4 | 配置失败（pilot 身份不符就在这一层 `FATAL_ERROR`） |
| 5 | 构建失败——**CTest 根本不会被调用**，所以不可能把上一轮的旧 exe 当成本轮结果 |
| 6 | 生成的测试清单与源码注册不一致，或选择为空 |
| 7 | 拿不到可判定的计数（CTest 没打印汇总行，或跑出的条数与导出清单不符） |
| 8+ | CTest 的真实退出码，原样返回 |

入口自带的三条反空转纪律：

1. **每次 CTest 调用都带 `--no-tests=error`**；带 `-Filter` 时先用
   `ctest -N --show-only=json-v1 -R <filter>` 确认匹配到的清单**非空**才去跑。
2. **判定看条数，不看日志里有没有 PASS**：脚本解析 CTest 自己的
   `N tests failed out of M` 汇总行，并要求 `M` 等于它导出的选择集大小。
3. **注册一致性**：无论是否过滤，完整清单都会与 `CMakeLists.txt` 里解析出的
   `NAME` 注册逐项比对两个方向，重名注册直接判失败——重复注册不算新增能力。

缺 `Rime_Install_Prefix` / `Baseline_Rime_Prefix`、或夹具文件缺失，仍在
**配置阶段** `FATAL_ERROR`。每个前缀下的 `rime_api.h` / `rime.lib` / `rime.dll`
都用 `NO_DEFAULT_PATH` 查找，并额外断言命中路径**落在该前缀之内**，
因此不会悄悄链接到机器上另一份 librime。

### 4.4 手工运行某个模式

只应在排查单个模式时手工运行，且必须用同一套判定包装件，不要自己发明第四种口径：

```sh
build/english-recovery/test/english-engine/rime_english_recovery.exe `
  --expect-dll <绝对路径>/english-engine/rime.dll `
  --fixture-dir <repo>/tests/fixtures/english_completion `
  --run-root <某目录>/rime-english-recovery-run/<leaf> `
  --mode completion
```

`--expect-dll` 必须是**绝对路径**：程序用 `GetModuleHandleExW(FROM_ADDRESS)` 反查真实
提供了 `get_version` 的模块再逐字比较，给相对路径会得到 exit 4 并打印 expected/actual。
这是设计，不是缺陷。`--run-root` 的父目录名必须是 `rime-english-recovery-run`，
否则程序以 exit 6 退出且**不删除任何东西**——这个守护就是防止误删别的目录。
用 PowerShell 手工单跑时**不要用 `Start-Process`**：它在含空格仓库路径上会把参数拆断，
五个模式一律 exit 2（这是脚本缺陷，不是测试结论，事故恢复里踩过一次）；
用 `& $exe arg1 arg2 …` + `$LASTEXITCODE`。

### 4.5 `-Skip` 的调用契约

`-Skip` 接受逗号列表，且经 `powershell.exe -File` 传入时也会被正确拆分
（`-File` 不会拆参数，脚本内部按逗号再拆一次）。未知名字被**拒绝**而不是忽略：

```
-Skip accepts marisa, yaml-cpp, leveldb, opencc, rime; got: <name>
```

`english-completion` 与 `component-probe` 两种模式**忽略** `-Skip` 中的四个依赖名：
它们从不重建依赖。

### 4.6 真实词表试验（P2.4，`pilot-lexicon`）

四步，全部离线（`--fetch` 是**唯一**会发起网络下载的一步，且只 GET、不上传）：

```sh
# 1) 取固定提交并转换（默认缓存 build/third-party-fetch/esdb，产物落 build/english-pilot/）
python tools/data/build_english_pilot.py --fetch
#    重转并要求逐字节相同： --source-root build/third-party-fetch/esdb --check
#    纯规则自测（不读源文件）： --self-check

# 2) 用**独立的二进制目录**跑，pilot 用例只在显式给出词表时注册
```

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 `
  -EnginePrefix   build/librime-english/rime/install `
  -BaselinePrefix build/librime-baseline/rime/install `
  -BuildDir       build/english-pilot-recovery/test `
  -PilotLexicon   <repo>\build\english-pilot\epocanva_en_pilot.tsv `
  -LogTag         <本轮后缀>
```

该选项**必须是绝对路径且文件必须存在**，否则脚本在参数阶段就退出；身份
（文件名与清单一致、字节数、SHA-256）由工程在**配置阶段**逐项核对并 `FATAL_ERROR`，
**绝不静默不注册**。不设置它，这个二进制目录里就只有手写夹具那批用例——
**不存在"静默跳过的 pilot 用例"**，报告必须写清自己跑的是哪一种。
脚本自己不实现第二种摘要，判定口径只有一套。

手工跑单个模式（`--pilot-lexicon` 只与四个 pilot 模式一起被接受，其他模式带它就报错）：

```sh
build/english-pilot-recovery/test/english-engine/rime_english_recovery.exe `
  --expect-dll <绝对路径>/english-engine/rime.dll `
  --fixture-dir <repo>/tests/fixtures/english_pilot `
  --run-root <某目录>/rime-english-recovery-run/<leaf> `
  --mode pilot-lexicon --pilot-lexicon <绝对路径>/build/english-pilot/epocanva_en_pilot.tsv
```

产物一致性由第二条用例判定：`cmake -DMANIFEST=… -DOUT_DIR=… -P
tests/english_pilot/assert_pilot_output.cmake`（见 §6.3）。

## 5. CTest 用例（pilot 关闭 **29** 条 = 26 条引擎 + 3 条摘要辅助；pilot 开启 **35** 条）

前缀一律 `recovery.*`。判定口径对每条都相同：**精确整数退出码 + marker 策略**，由
`tests/english_completion/assert_exit.cmake` 执行；**不使用 `WILL_FAIL`，也不使用
`PASS_REGULAR_EXPRESSION`**。下表"期望"列写的是退出码，`require` 表示必须出现
`RESULT PASS`，`forbid` 表示必须不出现。

这个包装件刻意与 P0.1 的 `tests/infrastructure/assert_probe.cmake` 并存而不合并——
后者只接受 ≤2 个参数，并且其注释说明这是有意的（丢掉第二个参数会让用例失去意义）；
英文集成程序需要 4–6 个参数，所以放宽旧包装件会削弱它为自己那批用例提供的保证。
新工程**复用**存活的 `assert_exit.cmake`，没有新增第二种判定口径。

### 5.1 手写夹具组（`tests/fixtures/english_completion/`，54 行词表，17 条）

| 注册名 | 可执行 | `--mode` | 期望 | marker |
| --- | --- | --- | --- | --- |
| `recovery.completion.prefix_candidates_and_commit` | english | `completion` | 0 | require |
| `recovery.completion.negative_baseline_engine` | baseline | `expect-module-absent` | 0 | require |
| `recovery.completion.positive_mode_fails_on_baseline_engine` | baseline | `completion` | **20** | forbid |
| `recovery.completion.negative_mode_fails_on_english_engine` | english | `expect-module-absent` | **18** | forbid |
| `recovery.completion.rejects_unknown_mode` | english | `not-a-real-mode` | **2** | forbid |
| `recovery.completion.suite_can_fail` | english | `self-check-fail` | **14** | forbid |
| `recovery.case.preservation_and_mapped_commit` | english | `case-preservation` | 0 | require |
| `recovery.case.suite_can_fail_on_menu_ranking` | english | `case-self-check-fail` | **14** | forbid |
| `recovery.token.boundaries_and_protection` | english | `token-protection` | 0 | require |
| `recovery.token.suite_can_fail_on_protected_text` | english | `token-self-check-fail` | **14** | forbid |
| `recovery.space.commit_and_multiword` | english | `space-commit` | 0 | require |
| `recovery.space.suite_can_fail_on_highlight` | english | `space-self-check-fail` | **14** | forbid |
| `recovery.acceptance.enter_and_space_semantics` | english | `acceptance-semantics` | 0 | require |
| `recovery.acceptance.suite_can_fail_on_highlight_return` | english | `acceptance-self-check-fail` | **14** | forbid |
| `recovery.lexicon.negative_missing_file` | english | `missing-lexicon` | 0 | require |
| `recovery.lexicon.negative_duplicate_entry` | english | `bad-lexicon-duplicate` | 0 | require |
| `recovery.lexicon.negative_illegal_word` | english | `bad-lexicon-illegal` | 0 | require |
| `recovery.lexicon.negative_illegal_word_form` | english | `bad-lexicon-illegal-form` | 0 | require |
| `recovery.lexicon.suite_can_fail_on_valid_data` | english | `lexicon-failure-self-check-fail` | **14** | forbid |

两条可执行文件来自**同一份模块化源码**，分别链接 english 构建与 baseline 构建的
`rime.lib`，并各自在自己的输出目录里放对应的那一个 `rime.dll`。第 3、4 条是**交叉注入**：
它们证明结果由实际加载的二进制决定，而不是由 `--mode` 字符串决定。
每组末尾的 `suite_can_fail_*` 是"测试必须能失败"的实证（`AGENTS.md` §3）：
它们各自断言一条夹具能证伪的行为，必须以 14 退出且不打印 marker。

**接受语义组（`acceptance-semantics` / `acceptance-self-check-fail`，P3.1）**在 completion
夹具上冻结 Enter 与空格的接受契约：高亮本身不提交；后续接受动作由按键语义决定
（见 §6.5）。它的控制条故意声称"高亮后 Return 提交原文"，必须以 14 退出。

### 5.2 拼写建议组（`tests/fixtures/english_spelling/`，16 行受控词表，7 条）

| 注册名 | `--mode` | 期望 | marker |
| --- | --- | --- | --- |
| `recovery.spelling.suggestions_and_suppressions` | `spelling-suggestions` | 0 | require |
| `recovery.spelling.commit_and_selection` | `spelling-commit` | 0 | require |
| `recovery.spelling.negative_lexicon_unavailable` | `spelling-lexicon-unavailable` | 0 | require |
| `recovery.spelling.suite_can_fail_on_suggestion_ranking` | `spelling-ranking-self-check-fail` | **14** | forbid |
| `recovery.spelling.suite_can_fail_on_labels` | `spelling-label-self-check-fail` | **14** | forbid |
| `recovery.spelling.feature_toggle_ab` | `spelling-toggle-ab` | 0 | require |
| `recovery.spelling.suite_can_fail_on_toggle_comparison` | `spelling-toggle-self-check-fail` | **14** | forbid |

`feature_toggle_ab` 是**开关对照**，不是又一组门槛断言：一个进程里暂存两套受控方案
（夹具那份开 `enable_suggestions`，另一份由程序派生、**必须只差两行**——schema 身份与开关），
跑完全相同的六个探针、每侧 25 次按键，然后用引擎自己的 `schema_open()` +
`config_get_*()` 读回两套**已部署**配置：开启侧读到 true、关闭侧读到"键存在且为 false"
（缺键与读成 false 是两种不同结论），而 `max_suggestions: 3`、`max_completions: 7`、
`menu/page_size: 8` 与词表路径两侧逐项相同。判定内容：开启侧出现文档约定的建议并带
`spell` 标签，关闭侧该输入只有原文（原本就是补全/抑制的输入则两侧菜单逐字节相同）；
两侧候选 0 恒为原文；两侧默认 Return 与默认空格提交的都是用户自己打的字节；
关闭侧 19 个 `spelling_*` 字段**逐个**为零并各自写明"若非零意味着什么"（而不是只压一个总和），
同时 27 个非拼写计数器在两阶段之间**必须一致**——这样"关闭侧很干净"就不可能靠"什么都没跑"挣来。
它的控制条 `suite_can_fail_on_toggle_comparison` 故意声称"两侧菜单一样"，必须失败。

### 5.3 摘要辅助组（不启动引擎，pilot 关闭时 3 条）

| 注册名 | `--mode` | 期望 | marker |
| --- | --- | --- | --- |
| `recovery.pilot_sha256.known_vectors` | `vectors` | 0 | require |
| `recovery.pilot_sha256.negative_identity_kinds` | `identity-negatives` | 0 | require |
| `recovery.pilot_sha256.suite_can_fail_on_wrong_vector` | `self-check-fail` | **14** | forbid |

第三条（可执行文件 `rime_english_recovery_sha256`）验证的是**测试工具自己**的摘要实现，
期望值来自仓库外三个互不相同的实现，因此它不是"被测代码给被测代码背书"。
这三种在 pilot 开关**关闭**时也注册——没有生成数据可跑，向量与负向种类照样成立。

### 5.4 核心单测（`build/vs2022-x64-debug`，Debug，4 条）

| 注册名 | 参数 | 期望退出 |
| --- | --- | --- |
| `english.core.lexicon_and_completion_unit_tests` | `--unit` | 0 + `RESULT PASS`（实测 1566 条断言 0 失败）|
| `english.core.unit_suite_can_fail` | `--forcefail` | **1** + 禁止 marker |
| `english.core.rejects_unknown_arguments` | `--not-a-real-mode` | **2** + 禁止 marker |
| `english.core.spelling_cost_trial_on_pilot_lexicon` | `--pilot-bench <tsv>` | 0 + `RESULT PASS`，**仅在生成的真实词表在位时注册** |

第四条是 P2.6 成本试验的**核心那一半**：它在 42,356 行常驻索引上按输入类别测量一次编辑
通路的开销，并用闭合算术式核对变体数。**引擎那一半从未存在**——事故前工程里注册的
`--mode pilot-spelling` 是先注册后实现，恢复工程既不实现也不注册它。
没有生成词表的机器上根工程只有 7 条，那一条是**不注册**，不是注册了然后跳过。

### 5.5 pilot 附加项（`build/english-pilot-recovery/test`，仅 §4.6 显式给出词表时注册，6 条）

| 注册名 | 期望 | 判定 |
| --- | --- | --- |
| `recovery.pilot.real_lexicon_reference_compared` | 0 + marker | `--mode pilot-lexicon`，42k 行上与独立参考实现逐帧比对 |
| `recovery.pilot.suite_can_fail_on_reference_ranking` | **14** + 禁止 marker | 真实数据上故意报错的次序 |
| `recovery.pilot.truncated_data_refused_before_the_engine` | **22** + 禁止 marker | 预校验在引擎读任何文件之前拒绝 |
| `recovery.pilot.absent_data_refused_before_the_engine` | **22** + 禁止 marker | 同上，另一种具名理由 |
| `recovery.pilot.output_matches_the_pinned_manifest` | CMake `FATAL_ERROR` 为失败 | `cmake -P tests/english_pilot/assert_pilot_output.cmake` |
| `recovery.pilot_sha256.real_lexicon_digest_matches_the_manifest` | 0 + marker | 用 Windows CNG 复核真实产物与清单一致 |

第一条**不是**第五条的重复：它证明引擎在 42k 行上的**行为**；第五条证明那份词表**就是
清单钉住的那一份**。二者可以独立失败——截断的文件仍能让引擎跑通（只是少一批词），
被改过的清单也能对上旧产物。

pilot 的注册是**附加**，不移除、不改写手写夹具的任何一条；开关关闭时上面这些名字在
CTest 里根本不存在，也就没有"静默跳过的 pilot 用例"这回事。

`english.pilot.spelling_suggestion_cost_a_b_on_pilot_data`（旧 `pilot-spelling` 性能模式）
**不在任何一张表里**：它在事故前就是注册先于实现、从未进入过任何一次绿色扫描，
现工程既未实现也未注册它。

## 6. `completion` 模式的判定口径

`mask` 单位是 librime 自己的修饰位（`src/rime/key_table.h:14-45`）；
本轮**只发裸键（mask 0）**。keysym 取自 `include/X11/keysymdef.h`。
`preedit[len=…]` 是**对 preedit 的字节偏移**（UTF-8 码元），
候选下标是**零基菜单序号**，计数器一律是**事件计数**。

| 步骤 | 断言（全部为读回值） |
| --- | --- |
| init 后、建会话前 | `module_initialized>=1`、`translator_constructed=0`、`load_attempts=0`、`queries=0` |
| 建会话 A 后 | `constructed>=1`、`live>=1` |
| `select_schema` 后 | `get_status().schema_id` 回读相符（**`select_schema` 返回值不作为判据**）；`attempts=1 ok=1 entries=54 status=ok no_path=0` |
| `h`/`he`/`hel` | 三行候选列表逐字相等 + 候选 0 为原文 + preedit 文本与**字节长度**分别断言 |
| Return（未选择） | `commit="hel"`，`composing=0` |
| 重打 `hel` → `select_candidate(1)` → Return | `select_candidate` 返回 True；**`commit="hello"`**；并断言 `commit != "hel"` |
| 重打 `hel` → BackSpace | 候选变 `he` 前缀 7 项；`queries` 增长 |
| Escape | `composing=0`、`preedit=-`、**`commit=-`**（无残留） |
| `en` | 8 项（1 原文 + 7 补全）触顶，`page_size` 回读为 8 |
| `zzz` | **1 项**（精确匹配去重） |
| 会话 B `app` | 4 项；同时回读 A 仍 `composing=0 preedit=-` |
| 终局 | `attempts=1`、`queries>=17`、`queries==original_candidates`、`with_index==queries-rejected`、`live=0`、`constructed==destroyed` |

汇总不变量 `queries == original_candidates`（实测 18==18）用来抓
"某次 Query 既没给原文也没报错"这类记账错误。
`queries >= 17` 的 17 是本轮注入的**字母键数**（A 三次 `hel` = 9、`en` = 2、
`zzz` = 3，B 的 `app` = 3），不是随手取的阈值。

## 6.1 `case-preservation` 模式的判定口径（P2.2）

与 `completion` 同一套读法：候选来自 `get_context().menu`，提交文本来自 `get_commit()`，
preedit 是 `composition.preedit`（`len` 为字节偏移）。差别只在**按键**。

大写按键的送法是被断言的事实，不是假设：
`Speller::ProcessKeyEvent()` 只用 `key_event.keycode()`（`src/rime/gear/speller.cc:104`），
全文件唯一读 `shift()` 的地方是空格键（`:107`），因此
**送 `shift + h`（keysym `0x68`、mask 1）得到的是小写 `h`**，大写必须送大写 keysym
（`H` = `0x48`）。该模式末尾就用一次 `shift+h` 断言 `preedit == "h"`，
防止后来者"顺手改成加 shift"而实际悄悄测的是小写。

逐形状期望（推导见 `tests/fixtures/english_completion/README.md` §5.1）：

| 输入 | 候选 | 默认提交 | 选 index 1 |
| --- | --- | --- | --- |
| `hel` | 4 项，全小写 | `hel` | — |
| `Hel` | `Hel, Hello, Help, Helmet` | `Hel` | **`Hello`** |
| `HEL` | `HEL, HELLO, HELP, HELMET` | `HEL` | **`HELLO`** |
| `H` | 8 项（首字母大写），触顶 | `H` | — |
| `HeL` | **仅原文 1 项** | `HeL` | 不存在 |
| `Hello` | **仅原文 1 项**（映射后重复被去重） | `Hello` | 不存在 |
| `HEL`→退格 | `HE, HELLO, HELP, HEN, HEL, HELMET, HERB` | — | — |
| A=`HE` / B=`he` 交替 | A 全大写、B 全小写，互不影响 | A `HE` / B `he` | — |

计数口径（`english_diagnostics.h`，P2.2 新增）：
`nomatch`（查了没中）、`mixed`（形状不补全）、`notalpha`（含非字母）、
`toolong`（超长）是**四个不同的桶**，加上 `fail`（词表加载失败）共五种"没有补全"的原因；
`rejected == mixed + notalpha + toolong`。
`cl / ci / cu` 分别是小写、首字母大写、全大写三种形状产出的补全数，
且 `compl == cl + ci + cu`（一条不变量，用来抓"产出了补全但没记形状"的记账错误）。
`attempts` 在整个大小写矩阵后仍为 **1**，即"改大小写不会重读词表、不重建索引"。

## 6.2 `token-protection` 模式的判定口径（P2.3）

读法与 §6/§6.1 相同：候选来自 `get_context().menu`，提交文本来自 `get_commit()`，
preedit 是 `composition.preedit`，`len` 是 UTF-8 码元（本模式全部输入为 ASCII，故等于字节）。
**没有任何一步由测试程序写 context**：`'`、`-`、数字与 `@ . _ : / \ ? & = # % +`
都作为自己的 X11 keysym 送进 `process_key()`（keysymdef.h:528-601 里可打印 ASCII 的
keysym 就等于码点），因此这条链路的每一步都是真实按键。

三条判据，各自独立，因为"最后提交看起来对"能同时掩盖后两条：

1. **可查询前缀**：`don'`、`co-`、`can'`、`re-` 各得 2 项（原文 + 词表行），
   `do` 得 5 项（`don't`(8) `done`(6) `does`(5) `doesn't`(4)，`do` 行被去重）。
   默认提交恒为原文；`select_candidate(index 1)` 后提交 `don't` / `DON'T` / `co-operate`。
2. **保护**：`user@example.com`、`https://example.com/a?x=1&y=2#z`、`C:\tmp\file.txt`、
   `foo_bar`、`v1.2`、`he2`、`hel.`、`don't2` 等，每一项都是 **cands=1 且 list 就等于整段原文**，
   `Return` 提交**逐字节相同**的原文。非法词形（`rock'n'roll`、`'hello`、`co--operate`）
   同样只出原文，但计入另一个桶。
3. **整段进入 translator**：`queries_covering_whole_input == queries` 且
   `queries_covering_part_of_input == 0`。这一条不能由候选列表代替——如果引擎把
   `hel@example.com` 切成 `hel` + 尾块，尾部补全可能出现在别的帧里而最终那一帧仍然正确。
   因此该模式**还逐帧断言**：从停止字节那一帧起，`compl` 增量必须为 0，
   而对应的原因桶恰好 +1（帧数 = 串长 − 停止字节偏移，逐条手算写在
   `tests/fixtures/english_completion/README.md` §5.2）。

`hel` → `hel@` 的中间态是这一节的核心证据：同一个 session 里候选从
`hel|hello|help|helmet` 塌成 `hel@` 一项，`protected` 计数 +1；
删掉尾部和 `@` 之后补全恢复，且恢复帧**恰好**重发 3 条补全。
退格恢复同理覆盖 `he2 → he`、`he. → he`，以及 65 字节的超长串
（`preedit[len=65]` 一字不减，`toolong` +1，退格回 3 字节后恢复补全）。

计数口径（`english_diagnostics.h`，P2.3 新增）：

```
rejected        = mixed + too_long + not_alphabetic + illegal_word_form
not_alphabetic  = non_ascii + protected + unsupported_byte
compl           = lowercase + initial_capital + uppercase
delimited       <= compl
whole + part    = queries
```

实测一次全模式跑完（`build/p23-verify/evidence-token-protection.log`）：

```
queries=405 with_index=405 rejected=138 orig=405 compl=399
nomatch=148 mixed=10 notalpha=108 toolong=1 illegal=19 protected=108
nonascii=0 unsup=0 cl=341 ci=42 cu=16 delimited=111 whole=405 part=0
attempts=1 ok=1 fail=0 entries=54 errline=0
```

`attempts` 仍为 1：新增的 5 行词条、三种大小写、撇号与连字符、以及 100 多次保护拒绝
全都走同一份进程内索引，没有任何一帧重读文件或重建索引。

## 6.3 `pilot-lexicon` 模式的判定口径（P2.4）

**读法与 §6/§6.1/§6.2 完全相同**（候选来自 `get_context().menu`，提交来自 `get_commit()`，
状态来自 C 诊断面），差别只在期望表按 42k 行重推。逐条口径：

| 步骤 | 断言 |
| --- | --- |
| `select_schema` 后 | `attempts=1 ok=1 fail=0 entries=42356 errline=0 status=ok`，且 `get_status().schema_id` 逐字回读为 `epocanva_pilot_english`（**返回值仍不作判据**） |
| `h` / `hel` / `inter` / `he` | 8 项触顶（1 原文 + 7 补全）；候选列表**逐字**比对，次序为 weight 降序、同分 ASCII 升序 |
| `don'` / `won'` / `re-e` / `co-op` | 分隔符前缀在真实词表上继续补全（`don'` → `don'\|don't`；`co-op` → 5 项含 `co-operate`） |
| `HEL` / `Don'` / `Co-op` / `HE` | 大写原样进组合区、原样出现在候选 0，补全按**同一形状**渲染（`HELLO`、`Don't`、`Co-operate`） |
| 选中 `don't` 后 Return | `commit="don't"`（大写键入时按输入形状提交） |
| 整词键入 `co-operate` | 候选**只有它自己**——已在词表里的词不再"补"出更长的同族词 |
| 10 条受保护输入（e-mail、URL、Windows 路径、`foo_bar`、`v1.2`、`co-operate.`、`hel2`、`don't2`、`rock'n'roll`、`'hello`） | 每条：候选只有整串原文、preedit 逐字等于原文、`completion_candidates` 增量 **0**、Return 提交逐字相等 |
| `co-oper` → `co-oper2` → BackSpace | 受保护前缀在真实词典数据上的恢复；另跑 `Aachen`（**查无此词**而非被拒绝）证明 6 字节原样提交 |
| 会话隔离 | 第二个 session 上 `Aachen` 的形状与提交不受第一个 session 的按键历史影响 |

两条只在 pilot 才成立的**结构**断言：

- `queries_covering_whole_input == queries`、`queries_covering_part_of_input == 0`
  ——每个 Token 都**整串**到达翻译器，保护不是"被切开之后没查到"；
  42k 行下这条仍成立，才说明 P2.3 的结论不是 54 行夹具的巧合。
- 计数恒等式（§6.2 的四式）在 `entries=42356` 上照样闭合。

**规模数字的口径**（`scale.*`，只打印、不参与判定）：

```
scale.cold_path  create session + deploy schema use + 第一次 Query（这一次 Query 里解析并排序整份清单）
scale.per_key    热索引下逐键均值（含 Return 提交帧），本轮 41 个注入键
scale.warm_path  第二个 session 的同样动作（索引已在进程内缓存）
```

边界与样本数（逐条写明，避免被读成基准系统）：

| 量 | 计时边界 | 样本数 | 已知包含的额外开销 |
| --- | --- | --- | --- |
| `cold_path` | `create_session()` 前 → 首键状态读回后 | **1 次观测**（非均值） | `select_schema`、引擎与 Translator 构造、首帧三次 API 读回 |
| `per_key` | 11 个探针输入的注入循环 ÷ **41 个键** | 41 个事件 → **1 个均值**（无分布/方差） | 每键的 `process_key` + get_status/get_context/get_commit 三连读、候选在 C 边界上的物化与复制 |
| `warm_path` | 与 `cold_path` 同一调用序列，第二个 session | **1 次观测** | 同上，但词表缓存已热（`attempts` 仍为 1） |

`cold − warm` 给出的是"解析 + 建索引"的**量级**，由两个单次观测相减得到，
因此同样带单次观测的抖动；`per_key` 不是引擎内部耗时，而是这套读法下的每键墙钟时间。
计时器是测试程序内的 `steady_clock`，未做进程隔离、未固定频率、未排除系统负载。

三者都是**本仓库构建树、单台 Windows x64 主机、一份 42k 行清单**上的一次实测：
不是生产基准、不与任何输入法可比、也不承诺换机器复现。
`per_key` 用**注入键数**做分母而不是"按键次数估计"，分母就打印在同一行。

## 6.4 `space-commit` 模式的判定口径（P2.5）

空格与连续多词输入的**规则**在 `engine/languages/en/english_space_policy.h`，
期望表在 `tests/fixtures/english_completion/README.md` §5.3。这里写"怎么读"。

**每个步骤同时断言五件事**，缺一不可——只看拼接字符串会漏掉一半故障模式：

| 读法 | API | 抓的是什么故障 |
| --- | --- | --- |
| 是否被处理 | `process_key()` 返回值 | 事件是否真的被引擎消费（拒绝的组合格望 `False`） |
| **原始组合** | `get_input()` | 组合里到底是什么字节。**不能用 preedit 代替**：preedit 出自 `Context::GetPreedit()` → `Composition::GetPreedit`，是展示加工结果 |
| 候选菜单 | `get_context().menu` | "空格自动接受了补全"会在选择前后菜单 diff 里现形 |
| 本次真实提交 | `get_commit()`，**循环读到空为止** | 拼接文本只由 API 返回过的字节组成，测试不补任何字符 |
| 提交次数与情形 | 组件诊断面 `space_case_a/b/c`、`composition_commits_issued`、`space_characters_emitted` | 见下方"为什么必须有计数" |

三条规则各自的期望（`hel`/`Hel`/`HEL`/`don'` 的菜单与提交逐项手推）：

```
A 有组合、无显式选择     → commit(原始 token) + 一个 U+0020，之后 composing=0 且 raw=""
B 已显式 select_candidate → commit(已确认的组合文本) + 一个 U+0020，只提交一次
C 组合为空               → 只提交一个 U+0020
```

**为什么必须有组件侧计数**：`Session::OnCommit` 是**累加**的
（`src/rime/service.cc:19,55-58`），所以一次空格产生的两次 sink
（文本 + 空格）在 `get_commit()` 处只读回**一个**字符串 `"hel "`。
"提交了几次"不能从文本推断，只能靠恒等式：

```
space_characters_emitted   = case_a + case_b + case_c
composition_commits_issued = case_a + case_b
space_events_seen          = case_a + case_b + case_c + space_events_refused
processor_key_events       = 测试侧发出的键数        （每个键恰好到达该 Processor 一次）
```

最后一条同时是**处理顺序**的证据：`epocanva_english_space` 在
`engine/processors` 第一位，非空格键一律 `kNoop` 放行，
所以引擎在 `src/rime/engine.cc:99-108` 的链上每个键都会经过它一次——
键数对不上就说明它要么被排到了后面，要么吞掉了不属于它的键。

**必须覆盖的边界**（都有具名断言，缺一即未验证）：
高亮不等于选择（`highlight_candidate(1)` 之后空格仍提交 `hel `）；
`Escape` 取消后空格不复活被取消文本；
Ctrl / Alt / Super / release 的空格一律不消费（`space_events_refused` +1、
提交队列为空、`handled=False`）；
两个 session 交替输入时**另一个 session 读回空**；
选择之后、空格之前提交队列必须为空（这就是 `select_candidate` 不提交文本的运行侧证据）。

## 6.5 `acceptance-semantics` 模式的判定口径（P3.1）

**冻结的契约：高亮不提交；后续接受动作由按键语义决定。** 五条规则，逐条在 completion
夹具上用真实按键与真实 `get_commit()` 验证：

```
1. 仅高亮（RimeApi::highlight_candidate）      → 无提交，组合不变，菜单不变
2. 默认高亮在原文（候选 0 = 输入本身）+ Return → 提交原文（"hel"），无空格
3. 高亮移到补全项 + Return                     → 提交当前高亮项（"help"），无空格
4. 仅高亮 + Space                              → 仍走 P2.5 原文策略："hel "（case A）
5. select_candidate 显式选择后                  → Return 提交所选文本（"hello"，无空格）；
                                                  Space 提交所选文本加一个空格（"hello "，
                                                  case B，维持现有行为）
```

三条依据都取自固定检出的真实源码，而不是本套测试的约定：

- `highlight_candidate` 的 API 契约本身就是"highlight a selection without committing"
  （`third_party/librime/src/rime_api.h:501-502`）。**高亮状态只能用
  `highlight_candidate()` 构造**；`select_candidate()` 是另一个状态（置
  `Segment::status`），不得用来冒充"仅高亮"。
- `fluid_editor` 把 `XK_Return` 绑到 `Editor::CommitComposition`
  （`src/rime/gear/editor.cc:176`）：先 `Context::ConfirmCurrentSelection()`
  （`src/rime/context.cc:168`，把段状态置 `kSelected` 并按
  `Segment::selected_index`——即当前高亮——取候选，`src/rime/segmentation.cc:51-53`
  读它时**不**查询段状态），段随之关闭，再 `Context::Commit()`。
- 空格由 EpoCanva 空格处理器先行决策（`engine/processors` 首位），它读的是段状态；
  高亮不动段状态，所以"仅高亮"到不了 case B。

产品引擎在本轮**零改动**：这五条是对现有行为的冻结与回归，不是重新设计——
不把 Enter 与 Space 强行统一，也没有新增词库。

控制条 `recovery.acceptance.suite_can_fail_on_highlight_return` 故意断言规则 3 的反面
（"高亮后 Return 提交原文"），必须以 14 退出：一个只看"提交了点什么"的 Return 测试，
与一个真正核对高亮项的测试，由此才可区分。

## 7. "词表没有每键重载"怎么读

看两个数的关系，不是看日志措辞：

```
attempts=1  ok=1  entries=54  queries=18  xl_ctor=4
```

`lexicon_load_attempts` 在 `EnglishLexiconCache` 的 **cache-miss 分支**才 +1；
`Query()` 内没有任何文件调用。`xl_ctor=4`（建会话与 `select_schema` 都会重建引擎）
与 `queries=18` 都远大于 1，而 `attempts` 恒为 1 —— 这就是证据本身。
**失败结果同样被缓存**，所以 `missing-lexicon` 模式也实测 `attempts=1`（而不是每次按键再开一次文件）。

P2.4 在同一份代码上把这个保证推到真实规模：pilot 模式终局读数为
`attempts=1 ok=1 fail=0 entries=42356 queries=183 with_index=183`，而
`translator` 生命周期是 `xl_ctor=4 xl_dstr=3 xl_live=1`——183 次查询全部落在**一次**
文件解析之上，42k 行没有换来第 2 次 `attempts`（该模式**显式断言** `attempts==1`，
并断言热索引第二次建会话后 `with_index==queries` 仍成立、
`xl_ctor>=2` 证明第二个 session 建的是它自己的对象）。
`xl_live=1` 是**预期值**：pilot 的终局读数取在销毁 pilot session 之前，
"销毁后 `live==0`"那条归零断言属于 `completion` 模式（§6 末行），
pilot 不复述它，也不假装它已在 pilot 路径上被验证。

## 8. 失败必须可识别（P2.1 立下的纪律，P2.2/P2.3 逐轮加细）

候选列表**单独看会骗人**：词表缺失、词表字节非法、词表词形非法（P2.3）、模块缺失、
查了没中、被保护策略拒绝、混合大小写拒绝、超长拒绝这八种情况
都可能表现为"仍有 1 个候选 = 我打的字"。因此判据永远是**计数 + 状态名 + 候选**三者一起：

| 情况 | `attempts/ok/fail/entries/errline` | `with_index` | 候选 | 另需 |
| --- | --- | --- | --- | --- |
| 词表不在位 | `1/0/1/0/0`，status `file-not-found` | 0 | 只有原文 1 项 | — |
| 重复词条 | `1/0/1/0/**3**`，status `duplicate-word` | 0 | 只有原文 1 项 | — |
| 非法词条（**字节**不属于词表） | `1/0/1/0/**3**`，status `illegal-word` | 0 | 只有原文 1 项 | — |
| 非法词形（**分隔符位置**不构成词条，P2.3） | `1/0/1/0/**3**`，status `illegal-word-form` | 0 | 只有原文 1 项 | 与上一行必须是**不同状态名**：字节合法但形状非法 |
| 模块缺失（baseline） | 诊断面整体 unavailable | — | **0 项** | `preedit="hel"` 且 `commit="hel"`，证明引擎没坏 |

最后一行的 **0 项**而不是 1 项，是"组件缺失"与"组件在场但词表坏了"的唯一区分点：
英文 schema **只列自有 translator**（不列 `echo_translator`），
所以没有该模块时没有任何东西能产出候选。

## 9. 隔离与可重建性

- 运行根必须是 `rime-english-recovery-run` 名下的叶子，否则程序 exit 6 并拒绝删除任何东西。
- `shared_data_dir`/`user_data_dir`/`log_dir` 全部指向私有目录；
  **不使用 `%APPDATA%\Rime`，不读个人配置**。
- 程序还会调用 `get_shared_data_dir_s` **读回**引擎实际使用的目录并与之比对——
  因为 schema 里的相对 `lexicon_path` 是按这个目录解析的，
  而 `Deployer` 的默认值是 `"."`（`src/rime/deployer.cc:16`）。
- 负向词表用**独立夹具文件**（`lexicon_bad_*.tsv`）在运行时复制成 schema 期望的文件名，
  写进自己的运行目录；`tests/fixtures/` 全程只读，不存在"改完忘恢复"。
- 三套引擎产物分目录、分进程：`build/librime-baseline/`、
  `build/librime-component-probe/`、`build/librime-english/`；
  `-Fresh` 在任一组件模式下只删除该模式自己的 `rime` 目录。

## 10. 阶段码

| 码 | 含义 | 码 | 含义 |
| --- | --- | --- | --- |
| 0 | 通过 | 11 | `get_status()` 失败 |
| 2 | 参数错误 | 12 | `get_context()` 失败 |
| 3 | `rime_get_api()` 返回 NULL | 13 | `get_commit()` 失败 |
| 4 | 实际加载模块 ≠ 期望 DLL | 14 | 至少一条值断言失败 |
| 5 | 夹具缺失 | 15 | destroy 后仍失败 |
| 6 | 运行目录准备失败 / 守护名不符 | 16 | `get_*`/`free_*` 未配对 |
| 7 | traits 未落到引擎（目录读回与请求不符） | 17 | 日志目录出现文件 |
| 8 | 部署返回值或产物不符 | 18 | **负向预期未成立**（引擎比声称的更能干） |
| 9 | `create_session()` 失败 | 19 | `process_key` 返回既非 0 也非 1 |
| 10 | schema 选择或读回不符 | 20 | **英文诊断面不可用**（模块或 `get_api` 缺失） |
| 21 | `get_counters()` 返回 False | 22 | **pilot 数据在引擎打开任何文件之前被身份预校验拒绝** |
| 25 | `select_candidate()` 被拒绝 | — | — |

任何 API 调用失败都直接返回对应阶段码，**不会用空值继续把测试跑绿**。
第 22 码与第 14、第 18 是三种**互不相同**的事实："这不是清单钉住的那份词表"、
"引擎真的加载了、某条预期与实测不符"、"那条应当拒绝的断言自己没成立"。
pilot 的两条设计为 22 的用例，其成功状态本身就是非零码，因此模式内部任何一条
自声明断言失败都会改报 **18**，绝不与 22 混读。
