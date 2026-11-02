# EpoCanva

EpoCanva 是一个以**英文能力为第一优先级**的输入法平台，底座为 librime（允许源码级改造），
最终交付 Windows 安装包（EXE）与 Android 应用包（APK）。

本文件只写**项目是什么**与**现在到哪一步**。
对所有协作者（含自动化）生效的永久规则在 `AGENTS.md`；
每个阶段的范围与验收标准在对应的任务书与 `docs/reports/` 阶段报告里，
**不写在本文件**，因为阶段性的限制会过期。

## 发行验证

项目自有代码采用 **AGPL-3.0-only**；第三方代码及数据保留原许可。当前仍是内部 Alpha，
不是已验收的五语言发行版。用户允许先做内部无签名验收，但不豁免功能、隐私、源码对应、安装与真实宿主测试。
发行条件与证据格式见
[`docs/product/release-gate.md`](docs/product/release-gate.md)。
产品构建入口 `tools/product/build_product.ps1` 按配置、构建、CTest 的顺序运行，
并记录完整源码身份、实际文件指纹、二进制 SHA-256、退出码及跳过项；
`tools/product/stage_product.ps1` 只接受匹配当前源码的成功构建收据，默认拒绝个人构建路径。
`tools/product/check_release.ps1` 分别检查 `internal-unsigned` 和 `public-signed`；
无签名模式只省略签名要求，五语言聚合、干净源码、源码归档、安装及宿主门禁仍然必需。
`technical_ready`、`distribution_authorized`、`signature_verified`、`release_ready` 分开报告。
包内文件校验通过不代表已获得分发许可或已完成系统输入验证。

## 现在能做什么，不能做什么

已具备五语言离线 n-gram 模型、真实 Rime 中文词典、Hunspell 法文字典、Mozc 数据驱动的日文解码和韩文两套式组字。后端不是只查询前缀：上下文会影响补全、下一词和多词缩写展开，候选选择采用独立的宿主提交确认协议。

本次审计新增兼容 v1 的 v2 profile 接口、非 ASCII UTF-8 输入、短期上下文导入、模型驱动首字母缩写、部分显式缩写、统一候选评分和前缀索引。US QWERTY、French AZERTY、German QWERTZ、Dvorak 与未知布局中性评分分开建模；语言、键盘布局和输出模式不再混为一个英文 schema。经过来源校验的本地暂存资源提供 13 个 profile，详见 `docs/testing/backend-resource-staging.md`。

**当前仍不可作为完整输入法交付。** Windows 新路径接入统一后端，但采用宿主保留原文、显式选择后替换目标词的模式，不是完整中日韩行内组合输入。产品持久学习尚因字段权限锁生命周期未闭合而禁用；简繁、部分确认、韩文词尾/空格、完整 Unicode 字素编辑和真实多键盘宿主矩阵仍未验收。没有 Android IME 发行物，当前源码的安装器与注册后跨宿主闭环也未完成。

本轮真实命令、测试结果、失败修复和剩余阻塞见 `docs/reports/ime-prediction-audit.md`。此前 44 项后端、228 项产品等记录属于历史源码，不能替代当前构建收据。语言声明保持 blocked/partial，不以核心或受控文本存储测试通过冒充正式宿主验收。

以下为既有英文与 Windows 路径的历史能力说明；P2.4 增加的是验证规模，而不是生产语料词频。

- 已有：librime 固定提交的原版构建与两套自有构建、公开 C API 上的无界面按键回放、
  编译进引擎 DLL 的自有组件及其可达性验证，以及
  **当前词前缀补全**：输入小写 ASCII 前缀，从本地测试词表查询，
  以真实 Rime 候选返回（候选 0 恒为原文），候选窗口按引擎页显示（每页 8 条）；
  方向键选择、Enter 接受当前有效选择、鼠标点击同样走版本校验后的选择路径；
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
- 尚未完成产品验收：生产词频与上下文**学习**、Unicode 英文完整性、
  基于实际语料的连写分段、跨应用一致性和下一词预测。
  新增形态、缩写、二编辑拼写是明确选择的建议，不自动纠正宿主文本；
  形态默认要求词典收录或所有格基词已收录，不声称完整词性分析。
  大小写方面**只有** ASCII 映射——没有 Unicode case folding、没有句首自动大写、
  没有专名/品牌词大小写恢复（`iPhone`、`GitHub` 一类仍不补全）；
  词形方面**只有**"至多一个 ASCII 分隔符、不在首尾、不两种混用"这一条有限规则——
  没有多分隔符词形、没有 Unicode 弯撇号/破折号规范化、没有自动标点替换、
  没有 URL/邮箱/代码解析器；**没有已安装、已激活的系统 TSF 输入法**，没有 Android
  前端，没有已执行的安装器；旧包的系统注册实验不能代替当前包的真实宿主验收；
  W1 只交付未注册的产品 TSF DLL 与设置壳；W2-A 已把产品 TSF DLL 接到真实
  `ITfKeyEventSink`、上下文观察、真实许可、协调器、固定 STA 唤醒与有界输入队列；
  W2-A-R 又补上高水位原样降级（文本不丢/不重/不乱序）、首 token 宿主归属与运行期共享锁；
  候选窗口已由 W2-B 实现；W2-B-R 修复了容量边界后的同会话恢复与 COM 回调/卸载生命周期，
  并补齐降级控制键与启动/停用端到端回归；W3-A 增加 UIElement 类别、宿主绘制决定遵从与
  token 锚点；W3-B 增加 `DllRegisterServer`/`DllUnregisterServer`、候选回退显示设置与
  版本/图标资源；安装和真实系统输入仍需与具体包身份绑定的完整 C 层证据。
- 词库事实：W1 起的内部产品包**已包含** `data/lexicon/epocanva_en_alpha.tsv`
  （42,356 行，由 `tools/data/build_product_lexicon.py` 从固定 ESDB 提交确定性生成并记录
  provenance），它随暂存包分发但**仅限内部 Alpha**——跨法域再分发仍需负责人与法务确认
  （`docs/compliance/english-pilot-data.md` §5）。默认测试夹具仍是 54 行手写词表；
  pilot 模式使用的 42,356 行清单是另一份**验证装置**，不是产品包里的文件。
- 因此此刻仓库能演示的输入仍极其有限：默认集成测试补全的是那 54 个词，
  但可以连续打多个词并用空格逐个提交（`Hello world `）；
  pilot 模式补全的是那 42,356 个词，但它是**验证装置**，没有前端、不在分发包里。
  在 `rime_raw` / `rime_probe` 两个夹具里候选文本仍与按键逐字节相同——
  那是链路证明；只有 `english_completion` / `english_pilot` 夹具的候选来自词表查询。
- W1 起另有一条**内部 Alpha 产品壳**：原生 Win32 的 `EpoCanva.exe`（设置、状态、离线/隐私说明、
  数据位置、许可入口）、产品 TSF DLL、`%LOCALAPPDATA%\EpoCanva` 用户数据、
  只读安装数据与白名单离线暂存包。W3-B 让 DLL 自带 `DllRegisterServer`/
  `DllUnregisterServer` 与图标/版本资源，但**当前源码对应的正式包尚未完成真实宿主闭环**；
  未注册时它不是系统输入法，候选窗口只在产品 DLL 自身的受控会话内出现。
  内部 Alpha 包不得公开分发。


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
| W1 | Windows 英文 Alpha 产品壳：设置/状态 EXE、产品 TSF DLL 配置与部署标记、离线数据与白名单暂存 | 已交付（内部 Alpha；未注册系统输入） | `docs/reports/W1-windows-alpha-product.md` |
| W2-A | 产品 TSF 生命周期接线：真实 key sink、上下文/许可/协调器、固定 STA 唤醒、有界输入准入、部署指纹与跨进程一致性 | 已交付（内部 W2 运行包；未注册） | `docs/reports/W2-A-product-input.md` |
| W2-B | 候选快照/选择校验、真实 TSF CandidateListUIElement 与自绘窗口、降级控制键、连续输入、异步引擎启动 | 已交付（内部 W2 运行包；未注册；C 层未验证） | `docs/reports/W2-B-candidates.md` |
| W2-B-R | 同会话容量恢复回归、降级 Enter/Escape/BackSpace 端到端、异步启动/停用/卸载生命周期、候选 UIElement 审查、隔离验证材料 | 已交付（内部运行包 `0.2.3-w2br`；未注册；C 层未验证） | `docs/reports/W2-B-R-crash-and-lifecycle.md` |
| W2-A-R | 输入可靠性收敛：高水位原样降级（原文优先于补全/拼写建议）、首 token 宿主归属、宿主默认输入与参考文档模型、运行期共享锁取证 | 已交付（内部运行包 `0.2.1-w2r`；未注册） | `docs/reports/W2-A-R-input-reliability.md` |
| W2-C | 输入保真 P0：普通原文归宿主、引擎建议有界可丢弃、显式候选替换的范围校验；删除 raw 容量路径并更正 177 的归属 | 已交付（内部运行包 `0.2.4-w2c`；未注册；C 层未验证） | `docs/reports/W2-C-input-fidelity.md` |
| W3-A | 正式 TIP 注册/激活身份、UIElement 类别、宿主绘制决定遵从、token 锚点与隔离验证工具 | 已交付（内部运行包 `0.2.5-w3a`；C 层 trace 轮次完成，正式包闭环待复验） | `docs/reports/W3-A-isolation-registration.md` |
| W3-B | `DllRegisterServer`/`DllUnregisterServer`（HKLM/HKCU、回读、回滚、外来 CLSID 拒绝）、候选回退显示设置、版本/图标资源、安装工具链只读探测 | 已实现并完成 B 层验证；C 层六轮真实会话已执行（2026-11-02）：注册/激活/卸载与零残留六次实证、TIP 已被真实宿主加载并打穿真实键入；**候选窗口真实可见仍未证明**（UAC 预算用尽） | `docs/reports/W3-B-install-and-fallback.md`、`docs/reports/W3-B-c-layer-sessions.md` |
| 发行准备修复 | 构建身份/收据、权限失败拒绝、运行库绑定、生命周期/共享锁、注册回滚、精确载荷/许可、路径清理与发行检查 | 产品 161 项：160 通过、1 显式跳过；内测候选已重建；公开发行仍阻塞 | `docs/reports/release-readiness-hardening.md` |
| 离线五语言后端 | 真实五语言资源、n-gram 预测、确认学习协议及训练评测 | 已有工程检查点；语言完整性、端到端性能与发行门禁未闭合 | `docs/reports/backend-prediction-development.md` |
| 预测输入法审计 | 上下文缩写、多 profile、键盘评分、资源暂存、统一后端 TSF 接线与回归 | 审计修正；不作为完整输入法交付，实际验收与阻塞见报告 | `docs/reports/ime-prediction-audit.md` |
| 之后 | 安装器、当前包 C 层闭环、完整中日韩组合与用户学习、其余英文能力、Android | 尚未交付 | — |

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
platforms/windows/
  product/                  W1 产品壳：EpoCanva.exe、产品 TSF 配置、设置/清单/诊断、GUI 与引擎测试
  tsf/ engine_bridge/ ...   P3 的 TSF 骨架、引擎桥、executor、coordinator 与既有测试
data/
  product/                  W1 产品 schema/default/NOTICE（只读安装数据源）
  manifests/                english-pilot.sources.lock.json：试验/产品词表的固定来源
tools/
  product/                  W1 构建入口（build_product.ps1）、白名单暂存（stage_product.ps1）、
                            make_app_icon.py（离线生成产品图标，无第三方素材）
  data/build_product_lexicon.py  W1 产品词表生成器（复用 P2.4 转换，独立 provenance，不联网）
  toolchain_probe/          C++17 工具链探针
  rime_lifecycle_smoke/     引擎生命周期冒烟（P1.2）
  rime_input_replay/        无界面按键回放（P1.3）
  rime_component_probe/     自有组件可达性测试（P1.4）
  test_english_integration.ps1  英文集成测试的正式入口：参数与工具检查 → 配置 → 构建 → 导出实际测试清单 → CTest → 真实退出码
  rime_english_recovery/    英文集成测试（事故后的模块化重建：入口 + 公共框架 + 每组行为一个文件；pilot 关闭 39 条 CTest，开启 45 条）
  rime_english_completion/  已退役的旧入口，目录内只剩指向新正式入口的 README（原因与旧用例去向见该文件）
  data/build_english_pilot.py  真实词表的确定性离线转换器（P2.4，默认不联网）
  build_librime_baseline.ps1  唯一的 librime 原生构建入口，-Mode baseline|component-probe|english-completion
  cmake/                    项目侧 CMake 适配输入（登记在 third_party/sources.lock.json）
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

### 4. Windows 产品壳与离线暂存包（W1，内部 Alpha）

前置：P2.4 的固定 ESDB 检出（默认 `build/third-party-fetch/esdb`，见
`docs/compliance/english-pilot-data.md`）与本机已验证的 librime 构建。产品词表**不直接复制**
pilot 文件，而是由独立工具重新验证固定提交后生成并记录 provenance：

```powershell
python tools/data/build_product_lexicon.py `
  --source-root build/third-party-fetch/esdb --out-dir build/product-data

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/build_product.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/stage_product.ps1 -Replace
```

产物在 `build/product/stage/EpoCanva-<version>/`：`EpoCanva.exe`、`epocanva_tsf.dll`、
`rime.dll`、`data/`、`licenses/`、`NOTICE.txt`、`VERSION.txt`、`package.manifest`。
`EpoCanva.exe` 从自身模块目录定位数据，从 `%LOCALAPPDATA%\EpoCanva` 读取设置/写入 Rime 用户目录；
可用 `--user-data-root <绝对路径>` 为自测重定向用户数据，`--diagnose <文件>` 输出键值状态报告。
暂存脚本只接受白名单文件，拒绝 PDB、测试 DLL、日志与文本路径；它对 rime.dll 内嵌的本机
`__FILE__` 构建前缀给出警告（公开分发前的构建修复项，见 W1 报告）。

**这不是已安装的系统输入法**：产品 DLL 现在导出 `DllRegisterServer`/`DllUnregisterServer`
（只写本产品自己的 CLSID、`InprocServer32` 与两个实现类别，支持 HKLM/HKCU、逐项回读、失败回滚）。
2026-11-02 的六轮 C 层真实会话（`docs/reports/W3-B-c-layer-sessions.md`）已在真实注册表上
六次执行注册/激活/卸载并逐项验证残留为零，产品 TIP 也已被真实记事本宿主加载并打穿真实键入；
但**候选窗口在真实宿主中的可见性尚未证明**，输入法选择器接管尚未演示，
也没有可用的安装器工具链；平时未注册时没有系统键盘接管，
候选窗口只在产品 DLL 的受控会话内出现。设置 EXE 可以保存并回读真实设置，
产品 TSF DLL 会在下一次被宿主加载/激活时部署设置补丁；不要描述成“系统输入已经可用”。
设置项之外的预测、学习和其他语言没有可点击的假开关。

## 许可证

项目自有代码采用 **AGPL-3.0-only**，保留根目录 `LICENSE` 与 `docs/product/license-decision.md` 的许可决定。第三方代码、词典、语料和模型各自保留原许可与署名要求；项目许可并不自动授予这些资料的再分发权。公开分发与稳定发行仍受独立验收门禁约束。
