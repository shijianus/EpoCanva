# `tests/fixtures/english_completion/` — 英文前缀补全夹具（P2.1）

消费方：`tools/rime_english_completion`（集成测试）与
`engine/rime_components/english/`（被编译进 `rime.dll` 的 Translator）。
运行方法与判定口径见 `docs/reports/P2.1-english-prefix-completion.md`（前缀补全）
与 `docs/reports/P2.2-english-case-preservation.md`（大小写映射与原文保真，§5.1 的期望表即本轮依据）。

本目录与 `tests/fixtures/rime_raw/`（P1.3）、`tests/fixtures/rime_probe/`（P1.4）互不覆盖：
那两个夹具的候选文本与按键逐字节相同，只是链路证明；
**本夹具的候选来自词表查询**，选择候选会提交与按键不同的完整单词。

---

## 1. 这个夹具证明什么，不证明什么

**证明**（全部由真实 `RimeApi` 返回值判定）：

1. `epocanva_english` 模块被 librime 自己的机制装载，`epocanva_english_translator` 被真实引擎构造；
2. 词表被读入并建成只读索引，查询结果作为**真实候选**出现在 `get_context().menu`；
3. 候选 0 始终是原始输入，其后是按 weight 降序、同分按 ASCII 升序的补全；
4. 与原文完全相同的词条不重复出现；
5. 直接按 Return 提交原文；**显式 `select_candidate` 选中一个补全后**提交该完整单词；
6. 退格后候选列表随之更新（`Query` 被重新调用），取消后没有残留提交；
7. 词表缺失 / 词表非法 / 模块缺失这三种情况会被**明确识别**，而不是被"仍能回显原文"蒙混过去。

**不证明**（每一项都是"未实现"，不得写成已通过）：

| 项 | 状态 |
| --- | --- |
| 拼写纠错、下一词预测、个性化学习、用户词库 | **未实现**；本组件不派生 `rime::Memory`，所以**它自己**不接词典与学习链路（这不代表引擎整体不写盘，`user.yaml` 一事见 `docs/testing/rime-english-completion.md` §9） |
| 撇号、连字符 | **P2.3 起支持有限词形**：`[a-z]+`、`[a-z]+'[a-z]+`、`[a-z]+-[a-z]+`，最多一个 ASCII 分隔符、不得在首尾、不得两种混用。`don'`、`co-` 这类"字母 + 一个尾随分隔符"是可查询前缀 |
| 技术性文本保护 | **P2.3 起生效**：数字与 `@ . _ : / \ ? & = # % +` 出现即**停止补全**，原文完整保留；这是保守补全策略，**不是**语法识别，也没有 URL/邮箱/代码解析器 |
| 完整分词、自动标点替换、Unicode 弯撇号与破折号规范化 | **未实现**；`he1lo`、`hel lo`、`rock'n'roll`、`'hello` 一律不补全，**只统计数，不改写、不裁剪、不提取局部、不清洗** |
| 词表规模 | **54 行**手写数据（P2.1/P2.2 的 49 行 + P2.3 新增的 5 行带分隔符词条）。**不是**词库，不代表生产覆盖，也不用于性能结论 |
| Unicode / 非 ASCII / 字素簇 / 屏幕列 | **未验证**，夹具数据全为 ASCII |
| TSF、Android、安装器 | **未实现** |

## 2. 七个文件

| 文件 | 作用 |
| --- | --- |
| `default.yaml` | 单条 `schema_list` + `menu/page_size: 8` |
| `epocanva_test_english.schema.yaml` | 唯一的 translator 是自有组件；**不列 `echo_translator`**，原文由组件自己放在候选 0。P2.5 起 `engine/processors` 的第一项是自有 `epocanva_english_space`（空格与提交边界），`speller`、`fluid_editor` 依次在后 |
| `lexicon.tsv` | **54 行** `word<TAB>weight`，`english/lexicon_path` 指向它（相对 shared data 目录）。其中 5 行带分隔符：`don't`、`doesn't`、`can't`、`co-operate`、`re-enter` |
| `lexicon_bad_duplicate.tsv` | 负向数据：第 3 行重复 `hello` → `duplicate-word` |
| `lexicon_bad_illegal.tsv` | 负向数据：第 3 行 `Help`（含大写）→ `illegal-word`，即**字节**不属于词表字母表 |
| `lexicon_bad_illegal_form.tsv` | 负向数据（P2.3）：第 3 行 `word-` → **`illegal-word-form`**，字节全都合法但分隔符位置不构成词条；与上一行是**两种不同失败**，各有状态名 |

两份 `lexicon_bad_*.tsv` **不被任何 schema 引用**。负向模式把它们复制进自己的运行目录、
冒充 schema 期望的文件名，因此正式夹具始终只读，不存在"改完忘了恢复"。

## 3. 词表契约（单位与取值）

**三个 `.tsv` 都是纯数据，没有任何注释行或空行。** 这不是排版偏好：本节描述的语法
明确规定"无注释语法、无空行"，`#` 开头的行会被判为 `missing-field`（没有制表符），
空行会被判为 `blank-line`。夹具若带说明性注释，第一次加载就会失败，
而且两份负向夹具的"第 3 行"会不再是第 3 行。说明文字只写在本 README 里。

与 `engine/languages/en/english_lexicon.h` 一致，逐条由 `tests/english_core` 断言：

- `word<TAB>weight`，LF 与 CRLF 均可；文件末尾允许恰好一个换行，**中间出现空行即失败**；
- `word` 非空、仅 `a`-`z`、≤ **64 个 UTF-8 码元（字节）**；
- `weight` 为十进制数字、无前导零（`0` 本身合法）、≤ 4294967295；
- 重复词条、缺字段、多字段、非法字符、溢出**各自有独立状态码**；
- 读取失败（文件不存在 / 不是普通文件）**不等于**"成功加载空词表"。

## 4. 候选预算与排序契约

- 总候选 ≤ **8**：候选 0 = 原始输入，其后 ≤ **7** 个补全；
- 排序：weight 降序 → 同分按 ASCII 字节升序（不用 locale、不用时间、不用无序容器顺序）；
- 与原文逐字节相同的词条被跳过，因此"精确匹配不重复"；
- 输入不被改写：超长（>64 字节）或含非 `a`-`z` 字节时**不补全**，只保留候选 0。

## 5. 手工推导的期望结果

以下每条都由本文件的人工推导得出，**不由被测函数生成**；
`tools/rime_english_completion/main.cc` 里逐字硬编码这些字符串。

### `hel`（4 个候选）

前缀 `hel` 命中 `hel(5) hello(10) help(10) helmet(4)`。
`hello`/`help` 同为 10，第 4 字节 `l`(0x6C) < `p`(0x70) → `hello` 先。
`hel` 与原文相同 → 跳过。

```
候选 0  hel        (原文)
候选 1  hello      (10)
候选 2  help       (10)
候选 3  helmet     (4)
```
**选择候选 1 → 提交 `hello`**（与按键 `hel` 不同，这才是补全生效的证据）。
不选择、直接 Return → 提交 `hel`（原文）。

### `en`（触顶 8 个候选，且验证精确匹配去重）

前缀 `en` 命中 17 行（`en` 自身也在其中），去掉与原文相同的 `en` 后剩 16 个补全，
预算只取 7 个。按 weight 降序：
9 分 `enable` `enjoy`（第 3 字节 `a`<`j`）；8 分有 5 个，第 3 字节序 `f`<`g`<`l`<`o`<`v`
→ `enforce` `engine` `enlarge` `enough` `envelope`。这 7 个正好占满预算，
被挤出的是 9 个：`ensure`(7) `entire`(7) `english`(6) `enlist`(6) `enemy`(5)
`energy`(5) `enormous`(3) `entry`(2) `envy`(1)。

```
候选 0  en         (原文；词表里的 en(4) 因与原文相同被跳过)
候选 1  enable     (9)
候选 2  enjoy      (9)
候选 3  enforce    (8)
候选 4  engine     (8)
候选 5  enlarge    (8)
候选 6  enough     (8)
候选 7  envelope   (8)
```
共 8 个，正好是预算上限；`ensure`(7)/`entire`(7) 权重低于某个 8 分词，因此不在此列。

### `he`（退格后；7 个候选）

命中 `he(2) hel(5) hello(10) help(10) helmet(4) hen(6) herb(1)`；`he` 与原文相同被跳过：

```
候选 0  he  (原文)   1 hello(10)  2 help(10)  3 hen(6)
候选 4  hel(5)       5 helmet(4)  6 herb(1)
```

### 其余

| 输入 | 期望候选（0 为原文） |
| --- | --- |
| `app` | `app`(原文)、`apple`(7)、`application`(7)、`apply`(4)；词表 `app(5)` 与原文相同→跳过；结果 **4 个** |
| `aaa` | `aaa` 命中且与原文相同→跳过，结果 **1 个**（词典序首位边界） |
| `zzz` | 同上，**1 个**（词典序末位边界） |
| `qq` | 无命中，**1 个**（仅原文） |

`app` 一行的推导：`app(5) apple(7) application(7) apply(4)` 四行都以 `app` 开头；
`apple` 与 `application` 同为 7，第 5 字节 `e`(0x65) < `i`(0x69) → `apple` 先；
`apply`(4) 其后；`app` 自身与原文相同被跳过。所以是 1 + 3 = **4 个候选**。

## 5.1 P2.2 大小写推导

映射规则（`engine/languages/en/english_case.h`）：词表仍只存小写词，
查询用小写键，**只有显示/提交文本**按原文形状变换。
形状判定顺序见该头文件；关键是单个 `H` 归 initial-capital 而不是"全大写"。

| 输入 | 形状 | 小写键 | 候选（0 恒为原文） | 不选择时提交 | 选 index 1 后提交 |
| --- | --- | --- | --- | --- | --- |
| `hel` | lowercase | `hel` | `hel, hello, help, helmet` | `hel` | `hello` |
| `Hel` | initial-capital | `hel` | `Hel, Hello, Help, Helmet` | `Hel` | **`Hello`** |
| `HEL` | uppercase | `hel` | `HEL, HELLO, HELP, HELMET` | `HEL` | **`HELLO`** |
| `H` | initial-capital（单字母） | `h` | `H, Hello, Help, Hen, Hel, Helmet, He, Herb`（8，触顶） | `H` | `Hello` |
| `HeL` | **mixed** | 不查询 | `HeL`（仅原文 1 项） | `HeL` | 无 index 1 |
| `Hello` | initial-capital | `hello` | `Hello`（映射后与原文相同 → 去重，1 项） | `Hello` | 无 index 1 |
| `HE`（由 `HEL` 退格） | uppercase | `he` | `HE, HELLO, HELP, HEN, HEL, HELMET, HERB`（7） | `HE` | `HELLO` |

推导要点：
- `hel` 范围 4 行 `hel(5) hello(10) help(10) helmet(4)`；10 分内 `hello` < `help`
  （第 4 字节 `l` 0x6C < `p` 0x70）；与原文相同的那一行被丢，所以三种形状都是
  **1 原文 + 3 补全 = 4 项**。
- `H` 的范围是 `h` 的 7 行（`he hel hello help helmet hen herb`），**没有**一行等于 `H`，
  所以不丢任何行，7 个补全正好占满预算 → 8 项；顺序按权重
  `hello(10) help(10) hen(6) hel(5) helmet(4) he(2) herb(1)`。
- `HE` 与 `he` 是**同一批 7 行的两种渲染**；`HE` 丢掉等于原文的 `he` 行 → 6 补全 + 原文 = 7 项。
- **去重在显示文本上做**：`Hello` 的映射结果就是输入本身，因此被丢弃，
  只剩候选 0。若在规范词上做就会漏掉这一情况（`hello` ≠ `Hello`）。
- 排名永远用**规范词**，所以大小写不改变顺序，只改变渲染。

`shift + h`（keysym `0x68`、mask 1）在本引擎里得到的是小写 `h`：
`Speller` 只用 `key_event.keycode()`（`src/rime/gear/speller.cc:104`），
唯一读 `shift()` 的地方是空格键（`:107`）。所以大写**必须**送大写 keysym，
集成测试按此发送，并把这一事实作为断言而不是假设。


## 5.2 P2.3 词形与保护推导

排序事实（决定下面每一条区间）：`std::string` 按**无符号字节**升序排，
`'` = 0x27、`-` = 0x2D 都**小于**任何小写字母（0x61）。因此 `co-operate` 排在 `coopt`
之前、`don't` 排在 `done` 之前。查询区间用**前缀后继**（把 key 的最后一字节 +1）
作为上界，`don'` 的上界是 `don(`：`done` 在第 4 字节是 `e`(0x65) > `(`(0x28)，落在界外。
P2.1 曾用的 `key + '{'` 只在"后续字节全 ≤ `z`"时成立，词表一旦含分隔符就变成运气，
故本轮换成与词表字母表无关的构造，并在核心单测里用**下标**逐项验证区间两端。

| 输入 | 判定 | 候选（手工推导） | 默认提交 | 显式选 1 号位后 |
| --- | --- | --- | --- | --- |
| `don'` | 可查询前缀（尾随分隔符） | `don'` \| `don't` | `don'` | `don't` |
| `Don'` | 同上 + initial-capital | `Don'` \| `Don't` | `Don'` | `Don't` |
| `DON'` | 同上 + uppercase | `DON'` \| `DON'T` | `DON'` | `DON'T` |
| `co-` | 可查询前缀 | `co-` \| `co-operate` | `co-` | `co-operate` |
| `CO-` | 同上 + uppercase | `CO-` \| `CO-OPERATE` | `CO-` | `CO-OPERATE` |
| `can'` | 可查询前缀 | `can'` \| `can't` | `can'` | `can't` |
| `re-` | 可查询前缀 | `re-` \| `re-enter` | `re-` | `re-enter` |
| `do` | 普通前缀，区间含 5 行 | `do` \| `don't`(8) \| `done`(6) \| `does`(5) \| `doesn't`(4) | `do` | `don't` |
| `don` | 区间 `don't`(8)、`done`(6) | `don` \| `don't` \| `done` | `don` | `don't` |
| `ca` | 区间 `cat`(8)、`can't`(7) | `ca` \| `cat` \| `can't` | `ca` | `cat` |
| `don't` | 完整词条；显示文本与原文相同 → 丢行 | `don't`（仅候选 0） | `don't` | — |
| `DoN't` | 混合大小写 | `DoN't`（仅候选 0） | `DoN't` | — |
| `Co-Operate` | 混合大小写（分隔符后出现大写即非受支持形状） | `Co-Operate`（仅候选 0） | `Co-Operate` | — |
| `word-` | 可查询前缀，词表无匹配行 | `word-`（仅候选 0），计数记 **no-match** | `word-` | — |
| `user@example.com` | **保护**（`@` 在偏移 4） | 原文 1 项 | 原文逐字节 | — |
| `https://example.com/a?x=1&y=2#z` | **保护**（`:` 在偏移 5） | 原文 1 项 | 原文逐字节 | — |
| `C:\tmp\file.txt` | **保护**（`:` 在偏移 1） | 原文 1 项 | 原文逐字节 | — |
| `foo_bar` | **保护**（`_` 在偏移 3） | 原文 1 项 | 原文逐字节 | — |
| `v1.2` | **保护**（数字 `1` 在偏移 1） | 原文 1 项 | 原文逐字节 | — |
| `he2` / `he.` | **保护**，退格删掉该字节后 `he` 恢复补全 | 保护期 1 项；退格后 `he`+6 补全 | 各自原文 | — |
| `rock'n'roll` | **非法词形**（两个撇号，第 2 个在偏移 6） | 原文 1 项，计 **illegal-word-form** | `rock'n'roll` | — |
| `'hello` | **非法词形**（首字符即分隔符） | 原文 1 项，计 **illegal-word-form** | `'hello` | — |
| `co--operate` | **非法词形**（重复分隔符，第 2 个在偏移 3） | 原文 1 项，计 **illegal-word-form** | `co--operate` | — |

要点，逐条被集成测试断言而不是写在这里就算数：

- **保护发生在停止字节及其之后的每一帧**。`hel@example.com` 里 `hel` 是正常可补全前缀
  （`hel|hello|help|helmet`），键入 `@` 的那一帧起候选塌成 1 项，
  计数 `queries_protected` 每帧 +1，且这些帧的 `compl` 增量为 **0**。
  退格删掉 `@` 后补全恢复，并断言恢复帧恰好重发 3 条补全。
- **`protected` 与 `illegal-word-form` 是两个桶**，`not_alphabetic` 是它们的粗桶：
  `not_alphabetic = non_ascii + protected + unsupported_byte`，
  `rejected = mixed + too_long + not_alphabetic + illegal_word_form`。
  混合大小写**绝不**记成词表问题，非法词形也**绝不**记成 `illegal-word`。
- **整段一个 segment**。`queries_covering_whole_input == queries` 且
  `queries_covering_part_of_input == 0`；若引擎把 `user@example.com` 切成
  `user` + 尾块，`part` 就会动，"只保留原文"就成了假象。
- 65 字节输入不查询，但 `preedit[len=65]` 逐字节保留，退格回 3 字节后补全恢复。

## 5.3 P2.5 空格与提交推导（`--mode space-commit`）

空格策略的**规则**在 `engine/languages/en/english_space_policy.h`，本文件只记录
"在这个 54 行词表上，每一步应当看到什么"。三条规则，逐条对应夹具里的行：

| 规则 | 状态 | 本夹具上的期望 |
| --- | --- | --- |
| A：有组合、无显式选择 | `commit(原文) + 一个 U+0020` | `hel` → 提交 `hel `（**不是** `hello `）；`Hel` → `Hel `；`HEL` → `HEL `；`don'` → `don' `（菜单里明明有 `don't`）；`user@example.com` → 整串 + 空格 |
| B：显式 `select_candidate` 之后 | `commit(已选词) + 一个 U+0020`，且只提交一次 | `hel` + 选 1 + 空格 → `hello `；紧接着再按空格 → 只有 ` `，不出现第二个 `hello` |
| C：组合为空 | 只提交一个 U+0020 | 连续两次空组合空格 → 两个空格；`Escape` 取消后再按空格 → 只有 ` `，被取消的 `hel` 不会复活 |

`he` 的菜单是 **7 项**（`he|hello|help|hen|hel|helmet|herb`），不是 6 项：
`herb(1)` 也以 `he` 开头，权重最低所以排最后。这条在 P2.5 第一轮里被写错过，
是集成测试把它抓回来的（`hel` 仍是 4 项，因为 `hen`/`herb` 不是 `hel` 前缀）。

### select_candidate 的提交时机（源码 + 运行两侧证据）

不要凭"选择只移动高亮"的旧印象写期望。固定提交上的实际链路是：

- `RimeSelectCandidate` → `do_with_candidate(..., &Context::Select)`
  （`src/rime_api_impl.h:1040-1043`），而 `RimeHighlightCandidate` 走的是
  `&Context::Highlight`（`:1035-1037`）——**两个不同的 Context 方法**。
- `Context::Select`（`src/rime/context.cc:118-130`）把 segment 置为 `kSelected`
  并发 `select_notifier_`；`Context::Highlight`（`:132-149`）只动 `selected_index`。
- `ConcreteEngine::OnSelect`（`src/rime/engine.cc:259-270`）关闭该段；若它已经覆盖到
  输入末尾则置 `kConfirmed`，并且因为 `fluid_editor` 把 `_auto_commit` 设为 false
  （`src/rime/gear/editor.cc:41-44` + `:189`），走 `composition().Forward()`
  ——**此刻没有任何文本提交**。
- 文本真正出去是在 `Context::Commit()`（`src/rime/context.cc:18-26`）→
  `commit_notifier_` → `ConcreteEngine::OnCommit`（`src/rime/engine.cc:251-257`）→
  `sink_` → `Session::OnCommit`（`src/rime/service.cc:19,55-58`，**累加**）。

运行侧同一件事的读法：选择之后、空格之前，本模式断言
候选菜单变成空（`Forward()` 之后 back segment 没有 menu）、
`get_input()` 仍是打过的原文、并且**提交队列里没有文本**。
`don't` 那一步因此是 `B` 而不是 `A`——`case_b` 计数 +1、`case_a` 不动。

### 为什么还需要组件侧计数

`Session::OnCommit` 是**累加**的，所以一次空格按键产生的两次 sink
（`commit 原文` 与 `U+0020`）在 `get_commit()` 处读回来是**一个字符串** `"hel "`。
"提交了几次"因此不能从文本推断，只能看
`composition_commits_issued` 与 `space_characters_emitted` 两个计数：
恒等式 `space_characters_emitted == A + B + C` 与
`composition_commits_issued == A + B` 就是"一次空格 = 一次文本提交 + 恰好一个空格"
的可观测形式。测试侧的 transcript 只由 `get_commit()` 真正返回过的字节拼接而成。

### 组合原始文本从哪读

断言用 `get_input()`（`src/rime_api_impl.h:1062-1070` 直接返回
`ctx->input().c_str()`），**不是** `preedit`：preedit 来自
`Context::GetPreedit()` → `Composition::GetPreedit`，是展示加工过的串
（可含 soft cursor 等），拿它当"原始组合"会把展示层当成输入层。
证据行里 `raw=` 与 `preedit=` 分开打印，就是为了让人看见两者是否一致。

## 6. 负向夹具的预期

| 情况 | 必须观察到 |
| --- | --- |
| 词表文件不在位（不复制） | 模块已装载、Translator 已构造、`lexicon_load_attempts=1`、`successes=0`、`failures=1`、`last_load_status="file-not-found"`、候选只有原文 1 个 |
| `lexicon_bad_duplicate.tsv` | `last_load_status="duplicate-word"`、`last_load_error_line=3`、`loaded_entry_count=0` |
| `lexicon_bad_illegal.tsv` | `last_load_status="illegal-word"`、`last_load_error_line=3` |
| baseline 引擎（无该模块） | `find_module("epocanva_english")` 为 NULL、`get_api` 不可用、`hel` 的候选**只有 1 个**（没有组件，也就没有原文候选）|

最后一行是关键：baseline 上没有任何 translator 产出候选，因此"候选数为 0"本身就是
组件缺失的证据；反过来，英文引擎上若 `queries_with_index=0` 而候选仍然只有一个，
说明的是**词表没加载**，不是补全在工作。
