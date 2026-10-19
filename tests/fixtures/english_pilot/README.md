# `tests/fixtures/english_pilot/` — 真实词表试验夹具（P2.4）

消费方：`tools/rime_english_completion --mode pilot-lexicon`。
本目录**只放配置**：词表数据不在这里，也不进版本库。

| 文件 | 行数 | 字节 | 作用 |
| --- | ---: | ---: | --- |
| `default.yaml` | 34 | 1,813 | 只列一个 schema；`menu/page_size: 8` |
| `epocanva_pilot_english.schema.yaml` | 126 | 7,239 | 与手写夹具同一条 inert 引擎链，词表路径指向暂存后的 42,356 行清单 |

数据来源、授权与转换政策：`docs/compliance/english-pilot-data.md`；
机器可读的锁定清单：`data/manifests/english-pilot.sources.lock.json`。
运行方法与判定口径：`docs/testing/rime-english-completion.md` §4.6、
`docs/reports/P2.4-english-pilot-lexicon.md`。

---

## 1. 与手写夹具的唯一差别是数据

把两个 schema 的**可执行配置行**（去掉注释与空行）逐行对比，结果固定为
**26 行里 3 行不同**：

```
schema_id:      epocanva_test_english        -> epocanva_pilot_english
name:           …prefix completion test      -> …pilot lexicon test
lexicon_path:   lexicon/epocanva_test_lexicon.tsv -> lexicon/epocanva_pilot_lexicon.tsv
```

前两行是身份，第三行是词表接合点。**其余全部逐字节相同**：
同一条 processors/segmentors/translators 链
（`epocanva_english_space`、`speller`、`fluid_editor`、`abc_segmentor`、
`fallback_segmentor`、`epocanva_english_translator@english`）、
同一个 76 字节 `speller/alphabet`、同一个空 `speller/delimiter`、同一个
`editor/char_handler: noop` 与 `bindings/space: noop`、同一个 `max_completions: 7`、
同一个 `page_size: 8`、同一个 `version: "0.0.0-p2.5-fixture"`。

> 这一节 P2.4 写的是"25 行里 4 行不同"。P2.5 给两个 schema 同时加了同一条 processor
> 列表项、又把 `version` 一起提到 `0.0.0-p2.5-fixture`，所以差集从 4 行缩到 3 行。
> **两个文件必须一起改**：只给手写夹具加空格策略而漏掉这里，pilot 测的就是另一种配置。

这不是"顺手抄了一遍"，而是本轮度量的**前提**：只有配置逐字相同，
"在 42,356 行上观察到的行为"与"在 54 行上观察到的行为"之间的差才只能归因于数据规模。
如果两处的 alphabet、page_size 或 max_completions 有任何一项不同，
本阶段就不配把自己的数字叫规模测量。

`alphabet` 那 76 字节为什么长这样（大写、数字、11 个技术符号、双反斜杠的 YAML 转义），
理由记在手写夹具的 schema 注释里，本文件镜像同一份理由，不重复展开。

## 2. 为什么另开一个目录（任务书边界 6、7）

- 手写 54 行夹具与它的旧用例**期望值一个字都不动**，包括那些只对 54 行成立的
  手工推导候选表。P2.6 只往每个旧 mode 的末尾**追加**了一条断言
  （19 个 `spelling_*` 计数之和为 0），它把"旧 schema 没开建议"变成读数，
  不改动任何已有期望。
- 试验用词表有自己的 schema_id：同一个 id 在本仓库出现两次，
  编译产物日志就会歧义——看不出某份日志描述的是哪一份词表。
- 自己的运行目录（`build/english-pilot/run/…`）、自己的 CTest 二进制目录
  （`build/english-pilot/test`）、自己的暂存文件名
  （`<shared>/lexicon/epocanva_pilot_lexicon.tsv`）。

## 3. 词表怎么到这里

测试程序按 `--pilot-lexicon <绝对路径>` 把生成的 TSV **逐字节暂存**到共享数据目录下
schema 所写的相对路径（相对路径由引擎按 `RimeTraits.shared_data_dir` 解析，
**不是**按进程工作目录，见 `src/rime/setup.cc:46-47`）。
暂存前后各算一次大小，程序打印 `pilot.lexicon.staged … (502333 bytes)`；
断言要求暂存大小与生成文件一致，所以"测的是被截断的副本"不可能发生。

**本目录里没有那份 502,333 字节的 TSV**：它是派生数据，留在 `build/english-pilot/`，
在项目许可证未定、再分发问题未决之前不进版本库（理由与哈希都记在清单的 `output` 段）。

## 4. 用例注册条件（不留"静默跳过"）

`tools/rime_english_completion/CMakeLists.txt` 只在
`-DEPOCANVA_ENGLISH_PILOT_LEXICON=<绝对路径>` 指向**存在的文件**时才注册 pilot 用例；
路径是相对路径或文件不存在 → **配置阶段 FATAL_ERROR**。
没有配置该选项的机器**根本看不到 pilot 用例**（既不失败，也不假装通过），
这是设计而非缺陷，报告里必须写清哪些机器上跑过哪些用例。

## 5. P2.6：为拼写建议另生成一份 schema

`--mode pilot-spelling`（§九 的引擎端到端耗时）需要在**同一份 42,356 行词表**上比较
"建议关闭"与"建议开启"。它不新增数据，而是让测试程序在暂存阶段从上面那份 pilot schema
**逐字节派生**一个孪生件，暂存为 `<shared>/epocanva_pilot_spell_english.schema.yaml`：

| 孪生件与原件的差异 | 值 |
| --- | --- |
| `schema/schema_id` | `epocanva_pilot_english` → `epocanva_pilot_spell_english` |
| `schema/version` | `0.0.0-p2.5-fixture` → `0.0.0-p2.6-fixture` |
| 追加两行（`english:` 段末尾） | `enable_suggestions: true`、`max_suggestions: 3` |

`english/lexicon_path` **保持原值**，所以两个 schema 解析到同一个暂存路径、
共用同一份只读索引 —— 这正是该模式要断言的事实：跑完两个会话之后
`lexicon_load_attempts` 仍然是 **1**。孪生件只改身份与两个开关、不改任何引擎链配置
（processors/segmentors/translators、`alphabet` 76 字节、`delimiter` 空、
`char_handler: noop`、`bindings/space: noop`、`page_size: 8`、`max_completions: 7`），
所以两侧每键耗时的差只能归因于建议通路本身。

生成而不是再抄一份文件，理由与 §1 相同：抄写会让"只差两个开关"退化成习惯。
程序在暂存前重新检查生成文本（旧 id 行消失、新 id 行存在、两个新键存在、
`lexicon_path` 仍指向共享词表），任一不满足即失败退出。

## 6. 本夹具不是什么

- 不是生产词库：42,356 行是**试验规模**，且来源是拼写词典清单。
- 权重不是词频：`weight` 是"该词被 Beale 几部源词典收录"的粗分层（实测取值 2–12）。
- 不实现纠错、预测、学习、连写切分、自动标点。
- 不承诺跨机器的时间数值：`scale.*` 是在本机 42k 行清单上的一次实测，
  换机器就要重测，也不能拿来和任何商业输入法比较。
