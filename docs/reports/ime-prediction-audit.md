# 多语言预测输入法审计与修正记录

## 状态：未通过完整产品验收，不交付为完成版

本轮审计范围由当前用户请求确定：输入法必须依据上下文理解当前缩写、预测后续输入，并支持多语言、键盘布局与变体。历史 W3-B 的“其他语言不在本轮”不能替代这次目标。本轮没有 commit、push、tag、PR、系统注册、提权或安装工具。保留既有工作树修改，尤其是 C 层隔离脚本与报告。

审计结论是：原专案的真实五语言后端与 Windows 产品路径分离；旧产品固定英文 Rime schema，普通文本直通、前缀词表补全，不能证明用户所要求的预测输入法。本轮已接入上下文预测的必要算法、资源与产品适配，但完整中日韩组合编辑、产品持久学习与当前正式包的真实宿主验收仍未闭合。不得以任何下述核心/受控测试结果宣布完整输入法完成。

## 1. 环境摘要

- Windows x64，Git Bash / Windows PowerShell 5.1，既有 VS2022 Professional / MSVC 19.44、CMake/CTest、Python 3.14。
- 起点分支 `feat/offline-multilingual-backend`，HEAD `71281f84b4f8c524659a54d46cb47e9faa73c776`；工作树起点已脏，本轮未清理或提交他人修改。
- `cmake` 不在 Git Bash PATH；实际使用已有 VS 随附工具。`command -v python`、`command -v powershell.exe` 找到工具，VS CMake 文件探测成功；没有安装或修改 PATH。
- 日志、测试数据、编译输出均以 `build/<preset>/` 为根。子任务显式 `py_compile` 产生的两个源码目录 `.pyc` 已按实测哈希移到 `build/ime-audit/python-cache/`，未删除其他缓存或文件。
- 只读探测找到 Word、Edge、Chrome 常规安装路径。WiX/candle/light/ISCC/makensis 不在 PATH；PATH 探测不是完整安装器工具链证明。记录在 `build/ime-audit/acceptance-tools-probe.log`，exit 0。

## 2. 已实现与审计修正

### 统一 ABI 与上下文

- v1 C ABI 布局不变，增加独立 v2 profile 结构、多资源 profile 打开、UTF-8 输入、上下文导入、单调 host-event 确认、带 profile 身份的快照。
- 同语言可有不同资源绑定；相同不可变模型/词典按完整身份共享，键盘布局切换不重复加载整个词库。未支持的 profile、缺转换器、改变已绑定身份明确拒绝。
- 选择、完整复制 commit、宿主实际写入、ACK 各自分离；短缓冲不完成复制，重复 ACK/event 被拒绝。未知词和换行切断旧上下文，不能跨未知跨度拼接 trigram。
- 普通宿主文字仅进入短期 ID 上下文，不因 `learning_enabled` 自动持久化。个人统计仍仅由有可信 ID 的候选且成功 ACK 驱动。后端实际学习存储重开回归与产品学习准入是两件事。
- `NEXT_UNIT` 的单位是完整模型词法 token，不是 Unicode 字素簇、码点或 UTF-8 字节。没有已验证的完整字素算法，不声称字素级编辑。

### 预测、缩写与排序

- 新缩写解码器按当前前缀/首字母与最多两个前文 ID 进行有界多词搜索，而不是固定候选列表。支持紧凑 initials、分隔前缀和带 SHA 绑定资源的完整/部分缩写；生成结果全部来自已准入模型词条。
- 显式资源键与推测 initials 具有可区分的 provenance。精确资源键保留候选优先级，同键候选仍按上下文排序；不篡改其概率值。
- 各候选来源使用模型概率一次，再加独立输入匹配特征，移除重复 unigram/个人频率加分。相同显示文本保留真正较好的上下文分数；Hunspell 的拼写惩罚不会被模型分数覆盖。
- 英文形态候选存在实际表面词时使用表面词 ID，不把 lemma 当作已输出词；未收录所有格保留明确限定的 lemma 回退。
- 前缀检索改用不可变词法区间排名索引，避免每个缩写搜索节点扫描大量无关词。有限 beam/budget 仍显式标记不完整。
- 中文 Rime 候选保留真实 native ordinal，按短期上下文重排后仍选择对应引擎项；无来源证明的显示文本不转成可学习 ID。日文转换结合真实 POS/词典成本与上下文。

### 多语言、键盘与数据

- 本地验证资源覆盖 en/zh/fr/ja/ko。13 个暂存 profile 包含英文多个键盘布局、法文 AZERTY、拼音/注音、日文 Romaji/Kana/Hiragana/Katakana/Kanji 和韩文两套式/原生 Hangul。
- 拼写键位模型区分 US QWERTY、French AZERTY、German QWERTZ、Dvorak；未知布局使用中性成本，不能静默假设 QWERTY。此处为算法评分，不等于已安装实际物理布局的验收。
- `stage_resources.py` 只使用现有本地模型/资料；核对 144 项来源输入，生成 55 个文件、13 行 profile manifest；英文 36 项、法文 4 项显式缩写全部解析到实际词条 ID，OOV 跳过 0。
- 验证根 `build/ime-resources/product`。`data/backend/resources.tsv` 为 2785 bytes，SHA-256 `f41c085855ccfa49735299026784a237c01edf6707a1e26d93523b83a56c0227`。相关署名/来源与分发限制见 `docs/testing/backend-resource-staging.md`。
- 不声称 en-GB 本土化词典、简繁转换或任意新语言已实现。日文假名-only profile 的 NEXT/CONTINUATION 缺少相应资源时明确 unavailable，不返回违背输出脚本的 Kanji。

### Windows 接入与安全

- 统一后端由既有单 worker executor 调用，协调器沿用焦点、字段权限、代际、候选 token、精确目标范围和宿主写入确认。
- 产品读取最多约 512 UTF-16 code units 的当前字段局部上下文，严格转换 UTF-8；不读全文件、不记录原文、不以按键计数冒充宿主写入成功。
- 当前产品模式仍是**宿主保留原文，明确选择后替换目标 token**。不是完整中日韩 TSF 行内 preedit；commit 同时保留 preedit 的形状仍拒绝，不能宣称部分确认完成。
- 产品个人学习禁用，因为可撤销的队列标记不等于能覆盖个人数据调用全程的真实字段权限锁。
- 资源路径加固拒绝 UNC、网络映射、设备别名、ADS、名称代理 reparse、脱机/回调取回文件等；本地常驻 OneDrive 元数据允许。目录句柄必须有真实读取权限，实际 rename 负向测试验证固定效果。
- Windows `LoadLibraryExW` 实测拒绝 GLOBALROOT/volume GUID DLL 路径（error 87）；最终加载用已固定的 DOS 路径并前后复核设备映射。并发 DOS 设备重映射仍是残余边界，不能宣称绝对无竞争。`ProductPaths` 返回后不自动延长所有后续 I/O 的固定寿命。
- Windows 普通进程内 TSF DLL 的离线保证是架构/依赖/代码约束，**不是 OS 网络沙箱**。Android 将来可通过不申请 `android.permission.INTERNET` 获得平台强制限制；本仓库本轮没有 Android 应用。

## 3. 设计取舍

- 复用现有模型、词典、单 worker、TSF coordinator 和范围校验，而不是新增一套绕过权限/ACK 的候选 UI。
- 本地 n-gram + 资源/initialism 搜索优先；未启用此前超时的神经重排器，不引入云请求。
- 保留宿主文本与显式选择原则，不通过自动改写或吞掉异步原始按键制造“输入法完成”的假象。
- 不放宽字素、简繁、语言完整性或 installed-host 门禁来让 readiness 变绿。缺资源/授权/证据即阻塞。

## 4. 实际命令与结果

机器专属工具路径不写入源码。以下 `CMAKE`/`CTEST` 指本机已存在的 VS2022 `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{cmake,ctest}.exe`；完整参数与输出保留在对应 build 日志。

| 执行 | 实际结果 | 证据 |
| --- | --- | --- |
| 未修改后端 configure/build/CTest，Python training/gate | 5 个阶段均 exit 0；基线后端 44 项 | `build/ime-audit/baseline-commands.json`、`baseline.xml` |
| 首轮新增后端 configure/build/CTest | exit 0；52/52、零跳过 | `build/ime-audit/backend-integration.xml` |
| 第二轮整合编译 | exit 1；并行编辑期间重复 RankedCount 声明，未执行后续 CTest | `build/ime-audit/backend-r2-build.log` |
| 第三轮完整编译 | exit 1；真实测试夹具 enum initializer-list 类型推导失败；已修复 | `build/ime-audit/backend-r3-build.log` |
| 已编译 ABI 复核测试 | exit 0；4/4 | `build/ime-audit/backend-r3-abi.xml` |
| 修复路径后完整后端 build/CTest | exit 0；56/56、零失败、零跳过 | `build/ime-audit/backend-r4.xml`、`backend-r4-ctest.log` |
| 英文 Debug / lexical Debug | exit 0；8/8、14/14 | `build/keyboard-english-tests`、`build/keyboard-lexical-tests` |
| prediction-core / abbreviation Release 与 Debug | 每套 exit 0、3/3；直接断言 32190 / 3875 | `build/prediction-prefix-index/*xml`、`build/prediction-abbreviation/*xml` |
| 路径加固相关 build/CTest | exit 0；13/13；聚焦路径 exit 0，故意失败 exit 14 | `build/windows-product/local-path-*-r4.log`、`local-path-negative.log` |
| `python -B tests/backend_training/test_training_evaluation.py -v` | exit 0；23 项 | `build/ime-audit/training-final-tests.log` |
| 最后命令绑定后的 gate 首轮 | exit 1；7 个失败源于 fixture 没有 CMAKE_COMMAND | `build/ime-audit/gate-final-tests.log` |
| 修正 gate fixture 后 Python gate | exit 0；13/13 | `build/ime-audit/gate-final-r2.log` |
| resources staging / check-only | exit 0；13 profile、55 文件 | `build/ime-resources/product/data/backend/staging-manifest.json` |
| staging Python | exit 0；37 项，36 通过、1 实际 symlink 权限跳过 | `tests/backend_resource_staging` 运行记录 |
| staged abbreviation native probe | exit 0；1/1 | `build/backend-resource-staging/native` |

已实际执行的主要后端命令：

```text
CMAKE -S tools/backend -B build/ime-audit-integration -G "Visual Studio 17 2022" -A x64 -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON
CMAKE --build build/ime-audit-integration --config Release --parallel 4
CTEST --test-dir build/ime-audit-integration -C Release --output-on-failure --no-tests=error --output-junit <repo>/build/ime-audit/backend-r4.xml
python -B tests/backend_training/test_training_evaluation.py -v
python -B -m unittest discover -s tests/backend -p test_*.py -v
python -B tools/backend/stage_resources.py --product-root build/ime-resources/product --oov error
```

### 测量的解释边界

相同生产模型、相同查询预算的前缀索引前后对照：法文 `m beau` 访问 32768→215 项，中位耗时约 6→0.24 ms；英文 `g mor` 32768→302、约 6.3→0.29 ms；韩文 `좋 아침` 32768→197、约 6.3→0.23 ms。详细命令/数据在 `build/prediction-abbreviation/public-probes-prefix-index-comparison.txt`。共享机器时序会变动；这些是缩写/core 查询，不是完整产品同步 p95/p99、额外 RSS 或母语质量通过证据。法文 beam 仍可能不包含目标词，韩文稀疏语料会给出语法不佳的高分短语；没有用手写 gold 或调大预算掩盖。

## 5. 产品验证结果

- 当前源码对应的后端验证 receipt 已生成：`build/ime-audit-integration/verification-20261102T234644/receipt.json`。配置、编译、CTest registration、CTest/JUnit、二进制引用和 source identity 均在同一验证脚本内检查，`verified_test_count=56`，各阶段 exit 0。后续源码变化会使 receipt 过期，不能手改续用。
- 英文独立集成 against current rebuilt Rime：exit 0，39/39，日志 `build/ime-audit/english-final.log`。
- 路径/运行库 focused suite：exit 0，13/13；故意失败控制 exit 14；没有注册或写入系统配置。
- Windows unified TSF r5 只完成构建目标，但 gated unified test 在后端启动阶段 exit 8；r6 全量编译暴露 `ExecutorOptions` 旧聚合初始化的 int→bool 收窄错误以及 unified test 字符串换行错误，尚未形成当前源码对应的全量 product CTest receipt。不得把 r5 目标 build 写成 product acceptance。

**未执行**：当前正式包的注册、Notepad/Chromium/Word 实际候选显示/选择替换、安装/升级/修复/卸载闭环；没有复用旧包六轮 UAC 授权。既有六轮证据的候选可见性仍未证明，本轮不把历史 literal 键入等同于功能验收。

## 6. 未完成与交付阻塞

1. 当前正式包的真实宿主候选与精确选择、跨键盘/变体、敏感字段、清理证据缺失；原逐轮 UAC 预算已用尽，本轮没有新的具体 session 授权。
2. 产品模式不是完整中日韩行内组合；多段、部分确认、字素编辑和对应交互尚未实现并验收。
3. 产品持久学习关闭，live permission 生命周期未闭合。后端存储测试不能代替该产品安全条件。
4. 简繁、中文词级预测、法文完整形态生成、日文母语转换/重写、韩文词尾与自动空格等仍部分实现。
5. 端到端尾延迟/预测额外内存、独立干净源码离线重建、可重链接依赖包、正式数据归属和分发审批仍阻塞。
6. Android 无实现；界面 i18n 未完成；不能把五语言资源或 13 个 profile 枚举当作全部语言/全部键盘能力。
7. 输入资源安全仍有明确 loader/后续 I/O 生命周期边界；静态导入扫描不能证明宿主进程绝无网络活动。

## 7. 文件清单

本轮项目路径如下（build 目录为证据/产物，不属于源码交付包）。任何 receipt 若因后续源码或二进制变化过期，必须重建重测，不得手改身份字段补签。

- `README.md`
- `data/backend/abbreviations/en.tsv`
- `data/backend/abbreviations/fr.tsv`
- `data/manifests/backend-readiness.json`
- `data/manifests/language-readiness.json`
- `docs/architecture/backend-session.md`
- `docs/product/requirements.md`
- `docs/reports/ime-prediction-audit.md`
- `docs/testing/backend-resource-staging.md`
- `engine/backend/CMakeLists.txt`
- `engine/backend/backend_api.cc`
- `engine/backend/context_text.h`
- `engine/backend/profile_api.inc`
- `engine/backend_session/session.cc`
- `engine/backend_session/session.h`
- `engine/include/epocanva_backend.h`
- `engine/keyboard/keyboard_model.h`
- `engine/languages/en/language_support.cc`
- `engine/languages/en/language_support.h`
- `engine/lexical_service/lexical_service.cc`
- `engine/lexical_service/lexical_service.h`
- `engine/prediction/abbreviation.cc`
- `engine/prediction/abbreviation.h`
- `engine/prediction/model.cc`
- `engine/prediction/model.h`
- `platforms/windows/engine_bridge/CMakeLists.txt`
- `platforms/windows/engine_bridge/rime_runtime.cc`
- `platforms/windows/engine_bridge/rime_runtime.h`
- `platforms/windows/engine_executor/backend_config.h`
- `platforms/windows/engine_executor/engine_backend.h`
- `platforms/windows/engine_executor/engine_executor.cc`
- `platforms/windows/engine_executor/engine_executor.h`
- `platforms/windows/engine_executor/engine_executor_internal.h`
- `platforms/windows/engine_executor/unified_engine_backend.cc`
- `platforms/windows/local_path/local_path.h`
- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/product_backend_resources.cc`
- `platforms/windows/product/product_backend_resources.h`
- `platforms/windows/product/product_engine_config.cc`
- `platforms/windows/product/product_paths.cc`
- `platforms/windows/product/product_paths.h`
- `platforms/windows/product/product_settings.cc`
- `platforms/windows/product/product_settings.h`
- `platforms/windows/product/settings_window.cc`
- `platforms/windows/product/settings_window.h`
- `platforms/windows/product/tests/product_unit_tests.cc`
- `platforms/windows/product/ui_ids.h`
- `platforms/windows/tsf/engine_host.cc`
- `platforms/windows/tsf/engine_host.h`
- `platforms/windows/tsf_coordinator/CMakeLists.txt`
- `platforms/windows/tsf_coordinator/backend_integration.cc`
- `platforms/windows/tsf_coordinator/candidate_snapshot.h`
- `platforms/windows/tsf_coordinator/coordinator.cc`
- `platforms/windows/tsf_coordinator/coordinator.h`
- `platforms/windows/tsf_coordinator/observed_binding.cc`
- `platforms/windows/tsf_coordinator/observed_binding.h`
- `platforms/windows/tsf_product/candidate_window.cc`
- `platforms/windows/tsf_product/host_context.cc`
- `platforms/windows/tsf_product/host_context.h`
- `platforms/windows/tsf_product/text_service_controller.cc`
- `platforms/windows/tsf_product/text_service_controller.h`
- `platforms/windows/tsf_product/unified_host_input.cc`
- `tests/backend/CMakeLists.txt`
- `tests/backend/c_consumer.c`
- `tests/backend/context_regressions.cc`
- `tests/backend/profiles.cc`
- `tests/backend/test_release_gate.py`
- `tests/backend_resource_staging/CMakeLists.txt`
- `tests/backend_resource_staging/main.cc`
- `tests/backend_resource_staging/test_stage_resources.py`
- `tests/english_core/language_support_tests.cc`
- `tests/lexical_service/CMakeLists.txt`
- `tests/lexical_service/main.cc`
- `tests/prediction_abbreviation/CMakeLists.txt`
- `tests/prediction_abbreviation/main.cc`
- `tests/prediction_core/main.cc`
- `tests/windows/engine_bridge/runtime_fault_tests.cc`
- `tests/windows/tsf_product/product_sink_tests.cc`
- `tests/windows/tsf_product/unified_backend_tests.cc`
- `tools/backend/CMakeLists.txt`
- `tools/backend/check_backend_release.py`
- `tools/backend/stage_resources.py`
- `tools/product/build_product.ps1`

起点已有且本轮没有覆盖的修改：`docs/windows/isolation-verification-plan.md`、`tools/product/isolation/layer_c/c_layer_agent.ps1`、`tools/product/isolation/layer_c/trigger.ps1`、`docs/reports/W3-B-c-layer-sessions.md`、`.zcode/`。README 的既有 C 层六轮说明也保留；新增审计状态不改写历史证据。
