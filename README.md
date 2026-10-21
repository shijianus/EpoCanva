# EpoCanva

EpoCanva 是一个以**英文能力为第一优先级**的输入法平台，底座为 librime（允许源码级改造），
最终交付 Windows 安装包（EXE）与 Android 应用包（APK）。

本文件只写**项目是什么**与**现在到哪一步**。
对所有协作者（含自动化）生效的永久规则在 `AGENTS.md`；
每个阶段的范围与验收标准在对应的任务书与 `docs/reports/` 阶段报告里，
**不写在本文件**，因为阶段性的限制会过期。

## 现在能做什么，不能做什么

引擎底座已经跑通，**英文能力只有四项，范围都很小**；P2.4 增加的是**验证规模**，不是能力项。

- 已有：librime 固定提交的原版构建与两套自有构建、公开 C API 上的无界面按键回放、
  编译进引擎 DLL 的自有组件及其可达性验证，以及
  **当前词前缀补全**：输入小写 ASCII 前缀，从本地测试词表查询，
  以真实 Rime 候选返回（候选 0 恒为原文），显式选择候选后提交**完整单词**；
- 已有（P2.2）：**ASCII 大小写映射与原文保真** —— `hel→hello`、`Hel→Hello`、
  `HEL→HELLO`、单个 `H→Hello`；`HeL`/`hEL`/`iPh` 这类混合大小写**不补全、只保留原文**；
  默认提交永远是用户自己打的原文，只有明确选择候选才提交映射后的词。
- 已有（P2.3）：**有限词形与受保护文本保真** —— `don'→don't`、`co-→co-operate`，
  并按输入形状映射 `Don'→Don't`、`DON'→DON'T`（首字母大写**只**大写整词第一个字母，
  不会把连字符后的字母也大写）；数字与 `@ . _ : / \ ? & = # % +` 出现即**停止补全**，
  `user@example.com`、`https://example.com/a?x=1&y=2#z`、`C:\tmp\file.txt`、`foo_bar`、`v1.2`
  原样保留、原样提交，绝不补全它的前缀或尾部。这是**保守补全策略**，不是语法识别。
- 已有（P2.4）：**真实词表的规模化验证**（不是第四项输入能力）—— 同一组件、同一断言口径
  跑在 **42,356 行**词表上。该词表的准确身份是 **Alt12Dicts 固定子集的本地词表试验**：
  ESDB 仓库一个固定提交里 `alt12dicts/2of12full.txt` 这一个文件的确定性转换结果，
  **不是** ESDB/SCOWL 的 35/50/60 尺寸变体导出，也不是 aspell/hunspell 词典。
  该清单**只用于测试**：由 `tools/data/build_english_pilot.py` 离线生成到 `build/english-pilot/`，
  **不进版本库、不打包、不是生产词库**；来源/授权/未决问题见
  `docs/compliance/english-pilot-data.md`，逐项事实见 `data/manifests/english-pilot.sources.lock.json`。
  词表权重是"被几部 Beale 源词典收录"的**粗分层**（实测 2–12），不是语料出现频率。
- 已有（P2.5）：**空格分词与连续多词输入** —— `Hello`+空格+`world`+空格 得到
  `Hello world `。三条规则：有组合且未显式选择 → 提交**原始 token** 再提交一个 U+0020
  （`hel` 空格得到 `hel `，**不会**顺手接受 `hello`）；已显式 `select_candidate` → 提交那个词
  **一次** 再一个空格（不会重复该词）；组合为空 → 只提交一个空格。
  词与词之间重新计算大小写、保护与候选；不保存已提交历史，不做跨词回删；
  Ctrl/Alt/Super 与按键抬起**不**触发提交路径；连续输入**不等于**连写切分
  （`helloworld` 仍整串保留）。实现形态是编译进 `rime.dll` 的最小英文 Processor
  （`engine/rime_components/english/english_space_processor.cc`），规则本身在
  `engine/languages/en/english_space_policy.h`，测试侧只拼接 API 真正返回过的 commit。
- 没有：生产词库（默认夹具仍是 **54 行**手写数据，P2.4 的 42k 行是**测试用**派生清单）、
  词频与上下文**学习**、
  形态变化（复数、`-s/-ed/-ing`、所有格、缩合）、拼写**纠错**、连写**自动分段**、
  缩写展开、跨应用一致性、下一词预测、候选编排与分页；
  大小写方面**只有** ASCII 映射——没有 Unicode case folding、没有句首自动大写、
  没有专名/品牌词大小写恢复（`iPhone`、`GitHub` 一类仍不补全）；
  词形方面**只有**"至多一个 ASCII 分隔符、不在首尾、不两种混用"这一条有限规则——
  没有多分隔符词形、没有 Unicode 弯撇号/破折号规范化、没有自动标点替换、
  没有 URL/邮箱/代码解析器；也没有 TSF / Android 平台前端与安装器。
- 因此此刻仓库能演示的输入仍极其有限：默认集成测试补全的是那 54 个词，
  但可以连续打多个词并用空格逐个提交（`Hello world `）；
  pilot 模式补全的是那 42,356 个词，但它是**验证装置**，没有前端、不在分发包里。
  在 `rime_raw` / `rime_probe` 两个夹具里候选文本仍与按键逐字节相同——
  那是链路证明；只有 `english_completion` / `english_pilot` 夹具的候选来自词表查询。

## 阶段状态

| 阶段 | 内容 | 状态 | 报告 |
| --- | --- | --- | --- |
| P0.1 | 工程骨架、基线文档、工具链探针、CTest 基础设施 | 已交付 | `docs/reports/P0.1-implementation.md` |
| P1.1 | 上游审计与依赖锁定（librime 1.17.0 / `33e7814`、子模块、Boost 头树、许可证） | 已交付 | `docs/reports/P1.1-upstream-audit.md` |
| P1.2 | librime 原生构建基线（Release/x64/静态依赖/共享 rime/`/MT`）+ 生命周期冒烟 | 已交付 | `docs/reports/P1.2-native-baseline.md` |
| P1.3 | 无词库夹具 + 无界面按键回放 + 真实输入链路证据 | 已交付 | `docs/reports/P1.3-input-replay.md` |
| P1.4 | 第一个项目自有 Processor 编译进引擎并验证可达性 | 已交付（含一次完整复现轮） | `docs/reports/P1.4-component-probe.md` |
| P2.1 | 英文当前词前缀补全：语言核心 + 自有 Translator + 真实候选与提交 | 已交付 | `docs/reports/P2.1-english-prefix-completion.md` |
| P2.2 | 英文 ASCII 大小写映射与原文保真（`hel`/`Hel`/`HEL`，混合大小写不补全） | 已交付 | `docs/reports/P2.2-english-case-preservation.md` |
| P2.3 | 英文词形边界（`don'`/`co-`）与受保护文本保真（邮箱、URL、路径、版本号不补全） | 已交付 | `docs/reports/P2.3-english-token-protection.md` |
| P2.4 | 真实英文词表试验接入：来源锁定 + 授权记录 + 确定性转换 + 42k 行上的引擎集成与规模测量 | 已交付 | `docs/reports/P2.4-english-pilot-lexicon.md` |
| P2.5 | 英文空格分词与连续多词输入（三条空格规则、真实 commit 逐事件读取、最小编译进引擎的 Processor） | 已交付 | `docs/reports/P2.5-english-space-boundaries.md` |
| P2.6 | 一次编辑拼写建议：门槛表、四类编辑、`spell` 标签列、预算与长度窗口、提交边界（默认不自动替换） | 引擎与夹具已交付；集成测试走正式入口 `tools/test_english_integration.ps1` | `docs/reports/integration-harness-recovery-status.md`（逐条能力的恢复覆盖矩阵） |
| 之后 | 其余英文能力、C ABI 边界、TSF / Android 适配、打包 | 未开始 | — |

## 目录结构

```
CMakeLists.txt              根工程（C++17 / CTest，工具链探针、英文核心单测与基础设施用例）
CMakePresets.json           本机可用的 configure/build/test 预设
AGENTS.md                   面向自动化协作者的永久规则
engine/
  rime_components/
    probe/                  P1.4 可达性探针组件（编译进 rime.dll）
    english/                P2.1 英文组件（模块 epocanva_english）：Translator + P2.5 空格 Processor
  languages/en/             英文语言核心：词表、大小写、词形(token)、空格与提交边界、前缀查询，不含 Rime 头
tools/
  toolchain_probe/          C++17 工具链探针
  rime_lifecycle_smoke/     引擎生命周期冒烟（P1.2）
  rime_input_replay/        无界面按键回放（P1.3）
  rime_component_probe/     自有组件可达性测试（P1.4）
  test_english_integration.ps1  英文集成测试的正式入口：参数与工具检查 → 配置 → 构建 → 导出实际测试清单 → CTest → 真实退出码
  rime_english_recovery/    英文集成测试（事故后的模块化重建：入口 + 公共框架 + 每组行为一个文件；pilot 关闭 27 条 CTest，开启 33 条）
  rime_english_completion/  已退役的旧入口，目录内只剩指向新正式入口的 README（原因与旧用例去向见该文件）
  data/build_english_pilot.py  真实词表的确定性离线转换器（P2.4，默认不联网）
  build_librime_baseline.ps1  唯一的 librime 原生构建入口，-Mode baseline|component-probe|english-completion
  cmake/                    项目侧 CMake 适配输入（登记在 third_party/sources.lock.json）
data/manifests/             english-pilot.sources.lock.json：试验词表的来源、固定提交、授权与产物身份
tests/
  infrastructure/           CTest 基础设施用例 + assert_probe.cmake 包装件
  english_core/             英文核心单元测试（4 条 CTest，内部 1566 条断言）+ 可失败用例；第 4 条需真实词表在位才注册
  english_completion/       assert_exit.cmake 包装件（精确退出码 + marker 策略）
  english_pilot/            assert_pilot_output.cmake：把生成的词表与锁定清单逐项对齐
  fixtures/rime_raw/        无词库回放夹具（只用上游已注册组件）
  fixtures/rime_probe/      自有组件可达性夹具
  fixtures/english_completion/  英文补全夹具：**54 行**词表、schema、三份负向词表、期望推导
  fixtures/english_pilot/   真实词表试验夹具：与手写夹具**仅词表不同**的 schema/default
  fixtures/english_spelling/  P2.6 拼写建议夹具：唯一打开 `english/enable_suggestions` 的 schema + **16 行**受控词表
docs/
  product/requirements.md   产品定位、语言优先级、隐私与验收原则
  architecture/             分层与模块边界
  adr/                      架构决策记录（0001 英文优先 … 0004 编译进引擎的组件）
  security/threat-model.md  隐私威胁模型
  compliance/               依赖与许可证审计、english-pilot-data.md（试验词表授权）
  build/                    从零搭建步骤、librime Windows 基线
  testing/                  每个测试入口的运行方法与判定口径（P1.3 / P1.4 / P2.1-P2.6，含英文集成测试正式入口）
  reports/                  阶段实现报告（environment.local.md 是本机专属、被忽略）
third_party/                外部依赖的引入规则与来源锁定
```

构建产物一律只落在 `build/<preset 或 mode>/`，源码目录与系统目录不受污染。
`build/` 整棵树**不得对外发布**：上游在共享构建下会把 `data/minimal/*`
（其中 `cangjie5.dict.yaml` 自述 `License: GPL`）复制进构建树，词库许可审查仍未完成。

## 快速开始

### 1. 仓库自带工程（工具链与基础设施用例）

**已验证开发路径**：Visual Studio 2022 Professional + CMake 3.31.6-msvc6
+ 生成器 `Visual Studio 17 2022` + `x64` + `Debug`。
命令需在 **Visual Studio 开发者环境**（`VsDevCmd.bat` / 开发者命令提示符）中执行，
因为本机 CMake 随 VS 分发、不在常规 PATH 里。前置条件与排障见
`docs/build/bootstrap.md`。

```sh
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error
```

（该套件现在是 **8 条**：4 条工具链基础设施 + 4 条英文核心单测。第 8 条
`english.core.spelling_cost_trial_on_pilot_lexicon` 只在生成的真实词表在位时才注册，
所以没有该产物的机器上是 7 条——不存在"静默跳过"的那一条。）

`vs2026-x64-debug`（生成器 `Visual Studio 18 2026`）是**可选路径，本机未验证**：
上游 CMake 4.2 发布说明确认该生成器自 4.2 起提供，而本机 CMake 3.31.6 没有它，
直接执行会 exit 1。test 预设不会自动构建，必须按 configure → build → test 三步执行。

### 2. librime 原生构建

```sh
# 原版基线（构建四个依赖 + librime 本体）
tools/build_librime_baseline.ps1

# 探针组件编译进引擎（复用上面已产出的依赖，不重建、不下载）
tools/build_librime_baseline.ps1 -Mode component-probe -Fresh -Verbose_Build

# 英文补全组件编译进引擎（同样复用依赖；不编译探针，因此没有 F6 行为）
tools/build_librime_baseline.ps1 -Mode english-completion -Fresh -Verbose_Build
```

三种模式的产物分别落在 `build/librime-baseline/`、
`build/librime-component-probe/` 与 `build/librime-english/`，互不覆盖，
且脚本会主动断言 baseline 的 CMake cache 里**没有**任何 `EPOCANVA_*` 条目。
运行方法与各阶段验收矩阵见 `docs/testing/` 下的三份文档。

### 3. 测试程序

`tools/rime_lifecycle_smoke`、`tools/rime_input_replay`、`tools/rime_component_probe`、
`tools/rime_english_recovery` 都是**独立的顶层工程**：它们必须链接 Release /
静态 CRT 的 `rime.lib`，而根工程是 Debug / 动态 CRT，分开配置可以避免两种 CRT 互相渗漏。
各自的命令与 CTest 用例名见上面 `docs/testing/` 里的文档。

英文集成测试**不要手工拼命令**，走正式入口：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 `
  -EnginePrefix   build/librime-english/rime/install `
  -BaselinePrefix build/librime-baseline/rime/install `
  -BuildDir       build/english-recovery/test
# 需要一并跑真实词表用例时，追加：
#   -PilotLexicon <绝对路径 build/english-pilot/epocanva_en_pilot.tsv>
```

该入口自带失败纪律：工具缺失就指名缺哪个并退出（不安装、不改 PATH），构建失败**不会**去跑
上一轮的旧可执行文件，零条匹配的用例一律判不通过，日志按 `-LogTag` 落到新路径且**拒绝覆盖**。

## 许可证

项目自身许可证**尚未确定**。在架构负责人确认之前，本仓库不添加任何授权声明、
LICENSE 文件或版权头承诺。`third_party/` 下的规则仅针对外部依赖，与本项目许可证无关。
