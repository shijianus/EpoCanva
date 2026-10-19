# 英文试验词表：来源锁定与授权记录（P2.4）

对象：**Alt12Dicts 固定子集的本地词表试验** —— 把真实英文词表接入已编译进 `rime.dll` 的
English Translator，用于规模化验证（不是生产词库）。本文只回答四个问题：**用了谁的哪一份数据、
凭什么授权、转换政策是什么、还差哪些决定**。构建依赖的许可证审查在
`docs/compliance/dependency-audit.md`，两者范围不同。

**这份数据到底是什么，不是什么**（P2.5 收尾时补写，因为把它笼统称作"ESDB 试验词表"会被误读）：

- 是：ESDB 仓库**一个固定提交**上、Alt12Dicts 包里**一个文件**
  （`alt12dicts/2of12full.txt`）的确定性转换结果，只用于本机构建与测试。
- 不是：ESDB/SCOWL 的 **35 / 50 / 60 号尺寸变体导出**，不是任何 aspell/hunspell 词典，
  不是多包合并词表，也不是运行期资产。本轮没有运行过上游任何 `Makefile`。

机器可读的同一份事实记录在 `data/manifests/english-pilot.sources.lock.json`；
**两处若冲突，以本文为准并同步修机器清单**（本文是给人读的授权说明，清单是给工具读的校验基准）。

判定口径沿用 `dependency-audit.md` 的图例：

| 记号 | 含义 |
| --- | --- |
| 已核实 | 在固定提交的实际检出树里打开过该文件并逐字读过引用行 |
| 未核实 | 没有取到、没有读到或没有权限判断；不作为合规依据 |

---

## 1. 来源身份（已核实）

| 项 | 值 | 证据 |
| --- | --- | --- |
| 项目 | English Speller Database（ESDB），SourceForge project `wordlist` | `https://wordlist.sourceforge.net/` 的 `<title>` |
| git 远端 | `https://git.code.sf.net/p/wordlist/code` | `https://sourceforge.net/rest/p/wordlist/` 的 `tools[]` 只有 1 个 git 工具，挂载点为 `code` |
| **固定提交（恢复身份）** | `7bbe293425d48a1210bf17aaf156b28ade5c2de1` | 在 `build/third-party-fetch/esdb` 内 `git rev-parse HEAD` |
| 该提交的 tag | `rel-2018.04.16`（**仅作描述**，不是身份） | `git tag --points-at HEAD` 返回且仅返回该名字 |
| 提交日期/标题 | `2018-04-16T22:11:56-04:00` / `Release 2018.04.16` | `git log -1 --date=iso-strict --format` |
| 检出树状态 | `git status --porcelain` 输出 **0 行** | 实际执行；本轮不修改上游任何文件（AGENTS.md §7） |
| 未采用的浮动引用 | `refs/heads/master` = `9998c28e2d5506357aa790a0c396b67eb907dc40`，比固定提交**领先 42 个提交** | `git rev-list --count HEAD..refs/heads/master` = 42 |

**为什么固定到完整提交而不是 tag 或分支**：tag 名可被维护者移动或删除，分支 tip 是"当前工作态"。
本轮实测该 tip 比发布 tag 多 42 个提交，属于没有发布声明的中间状态，
所以恢复身份只能是那串 40 位十六进制；tag 名字进清单只是为了说明"这个提交是什么"。

**取源边界**（P2.4 任务书边界 4、5）：`tools/data/build_english_pilot.py` **默认不联网**，
只有显式 `--fetch` 才发起 git 下载，且下载方向单向（只 GET），不上传任何本地内容；
转换本身对本地检出的检出树文件做只读哈希与解析。
`--fetch` 之后的三步校验（HEAD 相等 → porcelain 为空 → 逐输入 SHA-256）任何一步不过都退出非 0，
不存在"取到别的提交也照样转换"的路径。

---

## 2. 实际导出范围（已核实）

检出根：`<repo>/build/third-party-fetch/esdb`（只记仓库相对形状——绝对前缀属于本机，
AGENTS.md §10 禁止把机器专属路径写进受版本管理的文件）。

只有一个输入文件：

| 路径（相对检出根） | sha256 | 字节 | 行数 | 行尾 |
| --- | --- | --- | --- | --- |
| `alt12dicts/2of12full.txt` | `76dc8ed76ada4d4d0a67ffc2b81499a5c8a25f3f741df2f3a3d9bb438bf6eaf2` | 1,702,816 | 53,789 | CRLF（实测 53,789 对，无孤立 CR） |

为读授权与字段定义而**打开过但不转换**的文件（逐个哈希已核对）：

| 路径 | sha256 | 字节 / 行 | 读它的理由 |
| --- | --- | --- | --- |
| `alt12dicts/README` | `efec389c…` | 9,443 / 230 | 公有领域声明（28-29）、包身份（1、3、5）、**字段定义**（35-38、41-46）、AGID 指向（25-26） |
| `alt12dicts/README-orig` | `9576e583…` | 91,203 / 1,448 | 致谢请求与 AGID 例外（1441-1446） |
| `agid/README` | `0b1ca46a…` | 15,838 / 358 | 第 5 行的 Atkinson 版权——证明"排除 AGID"是有正文依据的，不是假设 |
| `scowl/Copyright` | `1420a34c…` | 12,006 / 248 | 证明仓库级许可标签不互相覆盖（见 §3.1） |
| `README.md` | `39948c69…` | 843 / 25 | 仓库级声明，只讲 SCOWL |

它是 **Alt12Dicts** 包（作者 Alan Beale，打包者 Kevin Atkinson，包版本按
`alt12dicts/README` 自述为 2016.06.26）里的"出现在多于一部 Beale 源词典中的词"清单
（`alt12dicts/README:34-35`）。

**明确排除的来源**（不是"没想起来"，而是逐条排除）：

- `alt12dicts/2of12infl.txt`、`alt12dicts/README-infl`、`agid/**`、`alt12dicts/README-agid`
  —— 被 README-orig 的 AGID 例外挡在公有领域之外。
- **ESDB/SCOWL 的 35 / 50 / 60 号尺寸变体**，以及任何 aspell / hunspell 词典、
  任何 `.dict.yaml` —— 一个都没取、没生成、没运行上游 `Makefile`。
- `scowl/`、`varcon/`、`pos/`、`ukacd/`、`yawl/`、`jargon-wl/`、`mwords/`、`census/`、
  `enable/`、`enable-sup/`、`other/`、`site/`、`uk-freq-class/`
  （该提交下只含 `notes.txt`）—— 各有自己的授权基础，需单独审查才能成为输入。

"它们同属一个词表仓库"不是扩大导出范围的理由。

---

## 3. 授权正文（逐字引用）

`alt12dicts/README:28-29`（已核实，9,443 字节 / 230 行，
sha256 `efec389cc088381843171c579f8358fe9d570537ec0e36badcf5e6c9751e1d73`）：

> All of these files have been explicitly placed in the Public Domain by
> Alan Beale.

`alt12dicts/README-orig:1441-1446`（已核实，91,203 字节 / 1,448 行，
sha256 `9576e583782d828e751f30940d1eea4ffa2e711df556ceb33075c7bd3c0dc915`）：

> The 12dicts lists were compiled by Alan Beale. I explicitly release them to the
> public domain, but request acknowledgment of their use. (Actually, the
> dependency of the 2of12inf list and the 2+2+3 lists on AGID prevents their
> release into the public domain. However, I do not impose any additional
> requirements on their use beyond those imposed by AGID and its sources, as
> described in agid.txt.)

两句合起来给出的使用基础：**公有领域 + 作者请求致谢**，并且**带一个作者本人写明的例外**。

- `README:28` 的 "these files" 指该 README 所描述的 Alt12Dicts 包文件；
  本项目的唯一输入 `alt12dicts/2of12full.txt` 正是该 README 第 32 行起逐段描述的清单之一，
  所以这句话覆盖到它。
- **AGID 例外已被遵守**：括号里说明对 AGID 的依赖使 `2of12inf` 与 `2+2+3` 系列
  **没有**进入公有领域。`alt12dicts/README:25-26` 把这份 AGID 关联指向 `2of12infl.txt`
  与 `README-agid`，而不是 `2of12full.txt`。因此 `alt12dicts/2of12infl.txt`、`agid/*`
  以及任何由 AGID 派生的文件**不在输入范围**，也没有被转换。
- 作对照：`agid/README` 自述 `Copyright 2000-2016 by Kevin Atkinson <kevina@gnu.org>`，
  与 12dicts 的公有领域声明是**两套不同的授权基础**。这条记录写在这里，就是为了防止后来者
  以"整个仓库同一授权"为由把 AGID 派生数据混进来。

### 3.1 一处更正：`scowl/Copyright` 实际存在

`README.md:9-11` 说 SCOWL "derived from many sources under a BSD compatible license"，
合并成果以 "MIT-like" 条款发布，条款正文在 `scowl/Copyright`。

本清单的早期草稿把 `scowl/Copyright` 写成"由 `make` 生成、该提交下不存在"。**这个判断是错的**，
已按实际检出树更正：该文件在固定提交上存在，12,006 字节 / 248 行 CRLF，
sha256 `1420a34cf9088f12bf9a837b5b0aa4b1affb553a1c66d82ba73a63b12cc80cec`。
其第 1-13 行是 Kevin Atkinson 的 2000-2016 版权声明，以及一段
"provided that the above copyright notice appears in all copies and that both that
copyright notice and this permission notice appear in supporting documentation"
的许可——**带署名条件的许可，不是公有领域**。

对本轮的直接影响：无（`scowl/` 没有被使用）。对后来者的影响：如果将来有人改用 SCOWL 派生清单，
义务与本文件的 12dicts 基础**不同**，必须另行记录与履行。

---

## 4. 转换政策（与授权同等重要）

由 `tools/data/build_english_pilot.py` 执行，全部决定写死在代码里并由
`tests/english_pilot/assert_pilot_output.cmake` 复校：

1. **不清洗后强行纳入**（P2.4 任务书边界 8）。不改大小写、不去标点、不截断、不重切分。
   一个不合格式就**排除并计数**。首次运行实测：源 53,789 行，纳入 42,356，排除 11,433，
   无法解析 0，且 `纳入 + 排除 = 源行数` 以及八类原因之和 = 排除数都由断言闭合。

   | 排除原因 | 数量 | 实际样本（取自源文件） |
   | --- | ---: | --- |
   | `contains-space` | 6,006 | `able-bodied seaman` |
   | `capitalised-letter` | 4,373 | `A` |
   | `digit-or-other-punctuation` | 831 | `A.` |
   | `more-than-one-delimiter` | 208 | `across-the-board` |
   | `leading-or-trailing-delimiter` | 15 | `'bout` |
   | `non-ascii-byte` | 0 | —（该文件是 7-bit ASCII，逐字节 `errors='strict'` 解码） |
   | `word-too-long` | 0 | —（实测最长词 23 个 UTF-8 code unit，上限 64） |
   | `duplicate-word` | 0 | — |

   规则按固定顺序逐条判定、**首个命中即计数一次**，所以 `A.` 只记入
   `digit-or-other-punctuation`、`able-bodied seaman` 只记入 `contains-space`；
   重跑不会改变归属。

2. **纳入的词形**就是引擎接受的词形：`[a-z]+`、`[a-z]+'[a-z]+`、`[a-z]+-[a-z]+`，
   至多一个 ASCII 分隔符、不在首尾，长度 ≤ 64 个 UTF-8 code unit。
   这条与 `engine/languages/en/english_token.h` 的 `IsDictionaryEntryForm` 是同一契约，
   因此"转换出来的词"与"加载器接受的词"不可能分叉。
   实测纳入构成：纯字母 40,590、含撇号 69、含连字符 1,697 ——
   后两类不为 0 是被断言强制的，否则这个"规模化"试验会悄悄停止检验 P2.3 的词形工作。

3. **权重不是语料词频**（P2.4 任务书边界 9）。字段来源是行首冒号前的那个整数：
   每行形状为 ` <N>: <非变形> <变体#> <非美式&> <次级=>   <word>`，
   `<N>` 即该词在 Beale 源词典中**被收录的部数**——
   这是包自己的定义，见 `alt12dicts/README:35-38`（五个数字依次的含义）
   与 `alt12dicts/README:41-45`（`7:  -  2#  5& -=   aeroplane` 的逐位解释）。
   本次实测纳入行权重取值范围 2–12，且最小值恰为 2 —— 与 README 自述"出现在**多于**一部源词典"
   一致，这既是"读的是第一个字段而不是别的字段"的旁证，也说明它是**粗分层**而非计数或概率。
   任何描述它的文字必须写"粗分层/收录部数"，不得写词频、n-gram 概率或用户使用统计；
   报告 JSON 里 `weights_are_corpus_frequency` 恒为 `false`，并由输出断言检查。

4. **确定性**：输出不含时间戳、绝对路径、主机名；按词的 UTF-8 字节升序；
   LF 行尾、无尾随空行、纯 ASCII。`--check` 重转一次并要求逐字节相同（本轮实测通过）。

5. **产物留在 `build/` 下**（AGENTS.md §10）：`build/english-pilot/epocanva_en_pilot.tsv`
   （502,333 字节，42,356 行，sha256 `6e46587d8029d8c9cf17b20d33983d051caa6e5e70a3da6573707d25400aae6b`）
   与 `build/english-pilot/epocanva_en_pilot.report.json`。**本轮不把它们提交进仓库**：
   它们是派生数据，可由固定提交重生成，而在项目自身许可证未定、§5 的两个问题未决之前，
   把派生数据固化进版本库等于替负责人做一次再分发决定。

---

## 5. 需要项目负责人决定的两件事（未决，不替其决定）

1. **公有领域是美国法概念**。在任何二进制或安装包离开本仓库之前，需要就产品目标法域
   给出再分发结论；EpoCanva 自身许可证仍未定（AGENTS.md §8），
   `third_party/sources.lock.json` 也已记录"迄今没有任何产物被批准对外分发"。
2. **打包时致谢以什么形式随数据走**。本轮只把致谢文本落进文档，不发明打包系统。

在这两点有结论之前，本试验数据的用途限定为**本机构建与测试**，这也是它不进版本库的原因：
生成物只落在 `build/english-pilot/`，**不进安装包、不进分发包、不作为运行时资产**。

同时固定一条纪律：**不给第三方数据追加我们自己签发的许可证声明**。
公有领域是"作者不再主张权利"，不是"我们可以替它选一个许可证"——
因此本轮没有、也不得给 `2of12full.txt` 的转换产物加上 CC0、BSD、MIT 或任何
EpoCanva 自己的许可标签（EpoCanva 自身许可证尚未决定，AGENTS.md §8，
所以连"给我们自己的产物加头部声明"这一动作本身都是越权的）。
授权记录的形式限定为两种：**逐字引用来源自己的文字**（§3）与**致谢**（§6）。

---

## 6. 致谢文案（可直接复用）

> English word list derived from the 12dicts lists compiled by Alan Beale, who placed
> them in the public domain and requested acknowledgment of their use; packaged in the
> English Speller Database by Kevin Atkinson at commit
> `7bbe293425d48a1210bf17aaf156b28ade5c2de1` (tag `rel-2018.04.16`), file
> `alt12dicts/2of12full.txt`. No AGID-derived list is used.

---

## 7. 本轮未做（因此不声称）

- **未做作者身份或签名验证**：该仓库的 tag 无 PGP 签名，`7bbe293…` 的匹配只证明
  "从该远端取到了这个提交对象"，不证明作者身份。
- **未审查上游生成脚本**（`alt12dicts/*.pl`、`Makefile`）：本轮直接读现成的
  `2of12full.txt`，不运行上游任何脚本，因此不声称该文件的生成过程被审查过。
- **未确认每个源词典自身的历史授权链**：Beale 的声明覆盖他编纂的 12dicts 清单；
  更上游（如 MWords 等）的逐条溯源不在本轮范围。
- **未取得任何书面再分发许可**，也没有联系版权方；§5 两项是开放决定。
- **未把任何词表纳入运行时或安装包**：本轮只到"引擎在 42,356 行上按真实按键产出候选"。
- **未做隐私面影响**：词表是本地静态文件，运行期无网络 I/O，不含任何用户文本
  （AGENTS.md §2 的架构性保证不变；本轮新增的读取路径全部在测试工具里，不在 TSF 客户端）。
