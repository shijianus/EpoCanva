# 多语言身份与自适应排序切片（阶段 B）

状态：**可编译、可测试、可失败；未接进产品路径。**
本文件只记录本轮实际做了什么、实际跑了什么。产品级结论见
`docs/reports/multilingual-intelligence-audit.md`。

## 1. 为什么选这两个切片

审计发现两个真实缺口，且都能在**不触碰已通过测试的 Windows TSF 实现**的前提下补齐：

1. **身份层缺失**：代码只认 5 个硬编码语言 id，`variant` 字段只做字符串比较，
   `zh-Hans-CN` / `zh-Hant-TW` 被直接拒绝，没有 BCP 47 解析与匹配，没有 region/script 变体。
   新产品规格要求 7 个核心语言 + 28 个声明语言 + 中文多变体身份。
2. **排序与学习语义缺失**：现有打分是若干未标定项直接相加
   （`lexical_service.cc:313-325`、`backend_api.cc:792-797`），
   学习存储只有正计数，没有负反馈、没有时间衰减、没有语言隔离键。
   规格 §5、§6 要求可扩展特征、有界负反馈、衰减与语言隔离。

被拒绝的替代方案：

- **不**在 `engine/backend/backend_api.cc` 里直接改现有分数：后端有一整套依赖当前排序的
  测试（上一轮报告记录为 56 项，本轮未重跑该套件），直接改会破坏既有证据，
  且违反"不一次性推倒已通过测试的实现"。
- **不**复制英文 Translator 为每种语言换词表：这正是规格禁止的做法。
- **不**引入 ICU：体积、许可、跨平台评估成本远超本轮需要；本轮只做身份与排序，
  Unicode 规范化继续由现有 `lexical_service/text.cc` 负责。

## 2. 新增文件

| 路径 | 作用 |
| --- | --- |
| `engine/locale/language_tag.{h,cc}` | BCP 47 子集解析、规范化、RFC 4647 lookup 匹配、默认文字、RTL 判定、七语言优先级 |
| `engine/locale/locale_profile.{h,cc}` | 输入 profile 注册表：scheme 三级实现状态、证据引用、确定性解析、8 个内置 profile |
| `engine/locale/CMakeLists.txt` | 独立可配置的静态库 |
| `engine/ranking/learning_events.{h,cc}` | 学习事件语义：显式选择/拒绝/被替代/手动词条，代际校验、归因校验、隐私拒绝、饱和、半衰期衰减、容量上限 |
| `engine/ranking/adaptive_ranker.{h,cc}` | 有界确定性排序：特征区间、按语言权重、原样候选硬分层、语言锁存、去重、拒绝而非截断 |
| `engine/ranking/CMakeLists.txt` | 独立可配置的静态库 |
| `tests/locale_registry/{main.cc,CMakeLists.txt}` | 3 条 CTest（unit / forcefail / unknown）+ 逐项断言 |
| `tests/adaptive_ranking/{main.cc,CMakeLists.txt}` | 13 条 CTest（12 个行为 + can_fail）+ `--demo` 可读证据 |

修改：

| 路径 | 改动 |
| --- | --- |
| `CMakeLists.txt` | 注册 `engine/locale`、`engine/ranking`、两个测试目录 |
| `engine/language_packages/CMakeLists.txt` | 链接 `epocanva_locale`；独立配置时自动引入 |
| `engine/language_packages/language_package.cc` | 语言身份与优先级改为委托 `engine/locale`，删除第二份硬编码列表 |
| `engine/include/epocanva_backend.h` | 追加 `EPOCANVA_BACKEND_ES=6`、`EPOCANVA_BACKEND_DE=7`，注释明确后端仍拒绝这两个值 |
| `tests/language_packages/main.cc` | 优先级断言更新为七语言顺序 |

## 3. 关键语义

### 3.1 身份

- `ParseLanguageTag` 只接受 language + script + region + variants；
  扩展与私有子标签**明确拒绝**（`kUnsupportedExtension` / `kUnsupportedPrivateUse`），
  而不是接受后忽略。
- `DefaultScript("zh")` 返回空：简繁是用户选择，不允许猜。
- `LookupMatches` 是子标签前缀匹配：`zh-Hant` 匹配 `zh-Hant-TW`，`en-GB` 不匹配 `en-US`。
- 解析出多个 profile（`zh` → 3 个）时返回 `kAmbiguous`，不任取其一。
- scheme 三级：`kImplemented` 必须有本地证据引用，`kUnverified` 有资源但无端到端证据，
  `kAbsent` 什么都没有。`Resolve` 只在 `kImplemented` 时返回 `kResolved`。
- 内置目录只列有真实实现路径的 profile。**Spanish / German 没有条目**，
  解析返回 `kNoProfile`——这正是"不得为凑数量只注册名称"的代码表达。

### 3.2 排序

- 每个特征先钳制到声明区间，再加权：
  prefix∈[0,1]、static∈[0,1]、context∈[-20,0]、personal∈[0,2]、negative∈[-1.5,0]、
  spelling∈[-1,0]、language∈[-1,0]。
- `context_logp = 0` 是"没有模型证据"的约定值；无模型时排序仍然有定义。
- **原样候选是硬分层（tier 0）**，任何分数都推不下去。这是"原文优先"在排序层的表达。
- 自动语言识别采用**锁存**：连续 N 次（默认 3）相同观察才切换；
  用户显式选择的语言只受有界语言项影响，不会被无界盖过。
- 输入超限返回 `kTooManyProposals` / `kInputTooLong`，**不静默截断**；
  非法 UTF-8 返回 `kInvalidUtf8` 且不产生部分顺序。
- 去重按显示文本精确字节，保留最高分条目。

### 3.3 学习事件

- **API 里不存在按键、提交或退格事件类型**。删除无法降低分数，因为无法表达。
- 候选级事件必须 `attributable == true` 且代际匹配，否则 `kIgnoredUnattributable` /
  `kIgnoredStale`，不写任何状态。
- 隐私：`kSensitive` → `kDeniedSensitive`；`kUnknown` → `kDeniedUnknownPrivacy`。
  未分类字段按拒绝处理，不按正常处理。
- 饱和：正向 ≤ 2.0，负向 ≤ 1.5。第三次与第五十次拒绝结果相同。
- 衰减：半衰期 1000 tick（调用方定义单位），`AdvanceTo` 不会倒流。
- 容量 512 条，超限先淘汰非手动词条，再按最旧更新与最小序号确定性淘汰。
- `SetEnabled(false)` 停止写入但保留既有偏好；`Clear()` 才是显式遗忘。

## 4. 实际命令与结果

工具路径为本机 VS2022 随附 CMake；`cmake` / `ctest` 不在 Git Bash PATH。

```text
CM = "C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/
      CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
CT = ".../bin/ctest.exe"
```

| 命令 | 实际结果 |
| --- | --- |
| `cmake --preset vs2022-x64-debug` | exit 0，配置完成 |
| `cmake --build build/vs2022-x64-debug --config Debug --parallel 8` | exit 0，无 warning |
| `ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error`（首轮） | **exit 8**；72 项中 3 项失败 |
| `epocanva_locale_registry_tests.exe --unit` | exit 1，3 条断言失败（大写规范化） |
| `epocanva_adaptive_ranking_tests.exe --learning` | exit 1，`actual 1.79875 expected 1.8` |
| `epocanva_adaptive_ranking_tests.exe --negative` | exit 1，饱和值差 0.00104 |
| 修复后全量 `ctest` | **exit 0；72/72 通过，0 失败，1 项显式跳过** |
| `epocanva_locale_registry_tests.exe --unit` | exit 0，`RESULT PASS` |
| `epocanva_adaptive_ranking_tests.exe --demo` | exit 0，输出见 §5 |

### 首轮三个失败的真实原因

1. **真实缺陷**：`UpperAscii` 写成 `ch - 'A' + 'a'`（应为 `ch - 'a' + 'A'`），
   导致 `zh-hant-tw` 规范化出非 ASCII 字节。测试直接抓到。
2. **测试口径错误**：三次选择放在 tick 0/1/2，衰减已经生效，我却断言未衰减的 1.8。
   改为同一 tick 后通过；衰减由独立用例验证。
3. 同上，负反馈饱和断言同样受衰减影响。

这三条都记录在案，没有通过放宽断言或删除用例来"修复"。

### 负向控制（证明用例不是无条件通过）

临时把 `kSelectionGain` 从 0.6 改为 0.0，只重建排序测试目标：

```text
cmake --build build/vs2022-x64-debug --config Debug --target epocanva_adaptive_ranking_tests
epocanva_adaptive_ranking_tests.exe --learning
```

结果：exit 1，三条断言转红（`repeated selection promotes the chosen candidate`、
`three selections accumulate 1.8 points`、`disabled model keeps existing preference`）。
恢复 0.6 后重建，全量 `ctest` 回到 72/72。

## 5. 运行证据（`--demo`，未编辑）

```text
before any explicit selection
  1. tier=0 score=3.15 text=coff source=completion
  2. tier=1 score=3.75 text=coffee source=completion
  3. tier=1 score=3.675 text=coffer source=completion
after selecting coffer three times
  1. tier=0 score=3.15 text=coff source=completion
  2. tier=1 score=5.47375 text=coffer source=completion
  3. tier=1 score=3.75 text=coffee source=completion
after four half-lives of decay
  1. tier=0 score=3.15 text=coff source=completion
  2. tier=1 score=3.78758 text=coffer source=completion
  3. tier=1 score=3.75 text=coffee source=completion
after clearing learned state
  1. tier=0 score=3.15 text=coff source=completion
  2. tier=1 score=3.75 text=coffee source=completion
  3. tier=1 score=3.675 text=coffer source=completion
```

读法：`coff` 是用户原样输入（tier 0），始终第一；`coffer` 被显式选择三次后从第三升到第二；
四个半衰期后优势缩小到 0.03758，已接近自然回落的 `coffee`；`Clear()` 后完全恢复默认顺序。

## 6. 没有完成的部分

- ranker **没有**接进 `epocanva_backend` 的候选生成，也**没有**接进 TSF 产品路径。
  因此不能据此宣称产品已经会学习或会智能排序。
- `LearningEventModel` 只在内存：**没有持久化、没有迁移、没有崩溃恢复**。
  持久化属于阶段 D，必须复用 `engine/data_store` 的原子写与隐私回调，
  不能另起一套文件格式。
- `engine/locale` 的 profile 目录是代码内表，还没有与
  `tools/backend/stage_resources.py` 生成的 13 个 profile 合并成单一数据源。
  两套清单目前可能不一致，这是下一轮必须消除的分叉。
- 权重是工程默认值，**没有离线评测**，不能称为"已校准"。
- 没有 CI：以上结果只在单机执行过。
