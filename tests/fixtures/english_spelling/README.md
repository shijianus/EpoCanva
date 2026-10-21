# `tests/fixtures/english_spelling/` — 拼写建议夹具（P2.6）

消费方：`tools/rime_english_recovery` 的五个拼写 `--mode`（经
`tools/test_english_integration.ps1` 运行）。
本目录**只有配置与那份受控词表**；期望值写在三处：本文件（推导过程）、
`tests/english_core/main.cc` 的 case 32–38（核心层字面量）、
`tools/rime_english_recovery/spelling_tests.cc` 等三个拼写文件（引擎端到端字面量）。

| 文件 | 行数 | 字节 | 作用 |
| --- | ---: | ---: | --- |
| `default.yaml` | 28 | 1,373 | 单条 `schema_list`；`menu/page_size: 8` |
| `epocanva_spell_english.schema.yaml` | 74 | 3,281 | 与手写夹具同链，另开 `enable_suggestions: true`、`max_suggestions: 3` |
| `lexicon_spell.tsv` | 16 | 157 | **受控测试词表**（不是英语词表），16 行 `word<TAB>weight` |
| `README.md` | 本文件 | — | 推导与不变量 |

---

## 1. 这份词表是什么，不是什么

它是**为测试设计的 16 行数据**：每行存在的理由是让四种编辑各有一个孤立例子、
让权重并列与预算截断可观察、让每个抑制门槛都有能命中它的输入。
`hillo`、`hallo`、`hollo` 这类行是**变体拼写或故意造的形近词**，不是词频统计，
也不是"我们收录了这些英语词"。权重同理：为了让排序断言有确定的期望序列而**手工指定**，
不来自任何语料，也不声称来自任何语料（P2.4 起这条纪律已固定）。

## 2. 全部 16 行

```
hello 9   help 8   hollo 7   hallo 6   hillo 6   hella 6   world 7   works 6
word  4   test 5   tests 3   hell 4    helium 3  don't 5   co-operate 2
abcdefghijklmnopqrstuvwxyzabcdef 1
```

最后一行 32 个字母，正好落在长度窗口的上沿；`don't` 与 `co-operate` 是
**带分隔符的行**，用来证明本轮从不建议这类词（它们无法由一次"字母编辑"产生，
但词表里存在它们，所以断言不是空转）。

`lexicon_spell.tsv` 的行序**故意不是字典序**：加载器必须自己排序，
把行序当排序依据的实现会在断言上露出来。

## 3. 期望值怎么来的（不允许调用被测代码）

一份**独立脚本** `build/p26-verify/derive_expectations.py` 逐行回答
"`word` 与 `key` 是否恰好相差本轮允许的某一次编辑"，再按权重降序、
规范词 ASCII 升序排序、按 `max_suggestions` 截断。它与被测实现走的是**相反的路**：

- 被测：由输入**生成变体**（删除 / 插入 / 替换 / 换位），再逐个 `Find()` 查表；
- 参考：拿**词表每一行**与输入做一对一的长度差判定（详见脚本 docstring）。

下面每个期望列表都是那趟脚本的输出，再抄成字面量。核心测试的 case 37 在运行时
用**第三种**独立实现（同一脚本的 C++ 翻译）对 50 个输入要求"集合 + 次序"完全相等。

## 4. 逐例期望（`max_suggestions = 3`）

| 输入 | 长度 | 结果 | 建议列表（规范词(权重,编辑)） |
| --- | ---: | --- | --- |
| `helo` | 4 | 有建议 | `hello(9,插入) help(8,替换) hell(4,替换)` —— `hello` 由两处插入得到，只出现一次 |
| `hxllo` | 5 | 有建议 | `hello(9,替换) hollo(7,替换) hallo(6,替换)`，第 4 个 `hillo(6)` 被预算截断（权重并列时 `hallo` 在前） |
| `hcello` | 6 | 有建议 | `hello(9,删除)` —— 只有删除到得了 |
| `ehllo` | 5 | 有建议 | `hello(9,换位)` —— 首对字母换位 |
| `wrld` | 4 | 有建议 | `world(7,插入)` |
| `wrod` | 4 | 有建议 | `word(4,换位)` |
| `wirld` | 5 | 有建议 | `world(7,替换)` |
| `wordd` | 5 | 有建议 | `world(7,替换) word(4,删除)` |
| `testz` | 5 | 有建议 | `test(5,删除) tests(3,替换)` |
| `helllo` | 6 | 有建议 | `hello(9,删除)` —— 三个 `l` 各删一个都得到它，结果只有一条 |
| `hxlo` | 4 | 无候选 | `hello` 距两次编辑，**不得**出现 |
| `zxqj` | 4 | 无候选 | 任何一行都不在一步之内 |
| `help` / `hello` / `test` / `tests` / `hell` / `world` / `works` / `helium` / `hillo` / `hallo` / `hella` / `hollo` / 32 字母行 | — | 词已在表 | 不搜索（`word-in-lexicon`） |
| `heli` | 4 | 有补全 | 普通补全 `helium` 存在 ⇒ 抑制建议，尽管 `help`/`hell` 只差一步 |
| `hEllo` / `HEllo` | 5 | 混合大小写 | 不猜 |
| `h3llo` / `h.elo` / `user@example.com` / `C:\tmp\file.txt` / `https://example.com/a?x=1` | — | 受保护 | 不纠正 |
| `don't` / `co-op` / `co-operate` | — | 带分隔符 | 本轮不进纠错（在"词是否已存在"之前判定） |
| `hel` / `hlo` | 3 | 过短 | 不搜索 |
| 32 字母行末位改 `q` | 32 | 有建议 | 那一条 32 字母的行，一次替换 |
| 32 字母行 + `z` | 33 | 过长 | 不搜索，且**不截断后再判** |

## 5. 引擎侧的候选预算

一次查询里**补全与建议互斥**：只要该输入有普通补全（`result.match_count > 0`，
即"去掉与原文重复的那一行之后仍有行"），就不产生任何建议候选。
因此菜单上限仍是 `1 + 7 = 8`：建议路径上最多 `1 + 3 = 4` 项。

判定"有没有补全"用的是 **k 截断之前**的行数，而不是列表长度 —— 否则一个把
`max_completions` 调到 0 的方案会把"有补全"误判成"没有补全"。

## 6. 大小写

三条被接受形状都用同一份数据测：`helo`、`Helo`、`HELO` 的规范词集合相同，
显示文本分别是 `hello/help/hell`、`Hello/Help/Hell`、`HELLO/HELP/HELL`。
`HELLO`（表内词的全文大写）走 `word-in-lexicon` 而不是建议，这一点也断言。

## 7. 关闭时等于没有本阶段

同一份词表把 `enable_suggestions` 去掉（或设为 false），菜单与 P2.5 完全一致、
所有 `spelling_*` 计数保持 0 —— 这句话现在在**三条**已注册的 CTest 用例上被读成数字：

- **同一份词表、同一串键入的开关对照**：`recovery.spelling.feature_toggle_ab`
  （`--mode spelling-toggle-ab`）。它在**一个进程**里跑两套受控方案：夹具这份开
  `enable_suggestions: true`，另一份由程序从夹具逐行派生、**只差两行**
  （schema 身份 + 那一个开关），两边 `english/lexicon_path` 解析到同一份 157 字节的
  受控词表。两侧都部署、都选中、都跑同样六个探针（每侧 25 次按键），然后：开启侧出现
  §4 约定的建议并带 `spell` 标签，关闭侧同样的输入只有原文（而原本就该走补全/抑制的
  输入两侧菜单**逐字节相同**）；两侧候选 0 恒为原文；两侧默认 Return 与默认空格提交的
  都是用户自己打的字节。关闭侧不是只看菜单——19 个 `spelling_*` 字段**逐个**为零，
  且每个都写明"若非零意味着进了哪条门"；同时 27 个**非拼写**计数器在两阶段之间必须一致，
  所以"干净"不可能是靠"那一侧什么都没跑"挣来的。开关的取值也不是读文件文本得出的，
  而是用引擎自己的 `schema_open()` + `config_get_bool()` 从**已部署**配置里读回来，
  "键不存在"与"键为 false"是两种不同结论。它的控制条
  `recovery.spelling.suite_can_fail_on_toggle_comparison` 故意声称两侧菜单一样，必须失败。
- `recovery.completion.*` / `case` / `token` / `space` / `lexicon` / `pilot` 各组
  （跑在**不命名** `enable_suggestions` 的 `tests/fixtures/english_completion/` 与
  `english_pilot/` 上）每个 mode 末尾都有一条 `SpellingTotal(c) == 0` 断言：
  19 个 `spelling_*` 字段（13 个状态 + 追加候选数 + 生成变体数 + 索引查表数 +
  3 种大小写渲染数）逐项求和必须为 0，也就是说"关闭"不是只看菜单没变，
  而是**整条建议通路一次都没进过**。这一条是**回归证据**，不是开关对照本身——
  它换的是另一份词表和另一份 schema。
- 核心层 `TestSpellingGateTable`（case 36，注册在
  `english.core.lexicon_and_completion_unit_tests` 这一条 CTest 用例里）直接调用
  `enabled=false`，要求返回 `disabled` 而不是任何一种"没有建议"。

一条容易混的边界，写在这里以免被读成"关掉开关就什么都不会替换"的反例：**高亮一行
然后按 Return，提交的是被高亮的那一行**。这是上游语义
（`Context::Commit()` → `ConcreteEngine::OnCommit()` → `Context::GetCommitText()` →
`Composition::GetCommitText()` 读 `Segment::selected_index`，而 `Context::Highlight()`
正是设它的），不是本阶段新增的行为。"高亮不等于接受"说的是**空格**走自有
`epocanva_english_space` 的 case A 时提交原文，与 Return 不是同一条路径。

## 8. 程序生成的 schema 不在本目录里

`spelling` 组还需要一条"词表读不出来"的路径，用来证明
`lexicon-unavailable` 不会被压成"没有拼写建议"。为此存在两份**由
`tools/rime_english_recovery` 在暂存阶段生成**的夹具，本目录不存放它们：

| 生成物 | 暂存位置 | 与原件的差异 | 由哪个暂存计划生成 |
| --- | --- | --- | --- |
| `epocanva_spell_broken_english.schema.yaml` | `<shared>/` | 只改两行：`  schema_id:` 与 `  lexicon_path:` 缩进键（换成 `…_lexicon_broken.tsv`） | `spelling-*` 四个模式 |
| 三行重复词表 | `<shared>/lexicon/epocanva_spell_lexicon_broken.tsv` | `hello 9 / help 8 / hello 7` —— 第 3 行重复第 1 行 | 同上 |
| `epocanva_spell_off_english.schema.yaml` | `<shared>/` | 只改两行：`  schema_id:` 与 `  enable_suggestions:`（true → false）；**词表路径与两个预算原样保留** | `spelling-toggle-ab` 与它的控制条 |

第三个生成物是 §7 那条开关对照的一侧。它对**逐行差异**有硬要求：行数必须与原件相等，
且不同的行**恰好两行**，否则以 `spelling.toggle-schema.rewrite` 失败退出——
"两套受控方案只差一个开关"是断言，不是约定。它的暂存计划也**不**生成上面那份坏词表，
用例为此专门断言两个路径在本此运行目录里**不存在**。

生成而不是再抄一份文件的理由：这两行差异是**断言**，抄写会让它变成习惯。
两处改写都用**缩进后的 YAML 键**做锚点，因为这份 schema 的头部注释里就写着
`# schema_id: epocanva_spell_english` —— 用松锚点第一次命中的是注释，
结果"文件名说 A、文件内 id 说 B"：librime 仍按内部 id 编译出**好的那一份**，
而新会话直接从 shared 目录读到坏词表，于是所有行为断言照样通过，
只有编译产物的名字暴露了真相。因此程序在暂存前会重新检查生成文本：
旧 id 行必须消失、新 id 行必须存在、坏词表路径必须存在，任一不满足即以
`spelling.broken-schema.rewrite` 失败退出。改写顺序也固定为**从后往前**——
新 id 比旧 id 长 7 字节，先改前面的锚点会把后面的偏移全部挪走。
