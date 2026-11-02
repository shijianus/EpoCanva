# 多语言基础与英文扩展验证记录（2026-11-02）

## 结论

**部分实现、部分验证；不是五语言交付。** 用户选择的 AGPL-3.0-only 和内部无签名验收已落实。英文形态、缩写/缩合和二编辑拼写已进入真实 Rime Translator，并通过设置投影到产品；基础词频/分段接口使用独立来源，不把 Alt12 收录次数冒充语料频率。五语言核心、纯 C ABI、学习存储均已新增并测试，但不能因此声称五语言 Windows 产品完成。

实际结果：

- 英文引擎构建及对象/CRT/导出/路径检查：最终 exit 0。
- 英文真实引擎：pilot 关闭 39/39、开启 45/45；全部通过，exit 0。随后同一目录从 pilot ON 切回 OFF，39/39 再通过，验证缓存开关清除及实际 JUnit 名称集合。
- 核心 Release 聚合：56 注册，55 通过、1 跳过、0 失败，CTest exit 0。
- 正式产品 Release：228 注册，226 通过、2 跳过、0 失败；configure/build/CTest 及构建入口最终 exit 0；当前源码收据已生成。
- 内部包：27 文件、4,823,278 字节，严格路径模式，无例外；暂存和隔离诊断 exit 0。
- 对应源码实际归档与最终门禁结果见本记录末节；不能把归档器单元夹具当作产品源码证据。

## 1. 环境、授权与范围

Windows 11 x64（10.0.26200），VS 2022 Professional / MSVC 19.44.35209.0，Windows SDK 10.0.26100.0，CMake/CTest 3.31.6-msvc6。Shell 是 Git Bash；PowerShell 5.1 通过 `powershell.exe` 显式调用。所有构建、试验、日志、用户数据隔离根均在仓库 `build/<preset>/`，未安装工具、修改系统配置、执行新 UAC/注册会话或 Git commit/push/tag/PR。

起始及当前 HEAD 为 `5c0f11b1fa94c965e1a00f920272860c2d20ff73`，工作树保留前轮 hardening 与其他会话改动。独立 `tools/product/isolation/` 改动和 `docs/reports/W3-B-c-layer-sessions.md`、`docs/windows/isolation-verification-plan.md` 不归本轮所有，未覆盖，也未将其旧包证据转换为当前包验收。

用户明确选择 AGPL-3.0-only，覆盖 AGENTS 原“未定”条款；已仅据此同步 §8，不替第三方作品改许可。既定语言优先级保留 English > Chinese > French > Japanese > Korean。获批计划末尾中文后日/韩/法顺序与永久优先级不一致，本轮按永久优先级组织产品能力门禁，独立组合算法允许并行准备。

没有擅自选择或下载中文/日文/法文词典，也未将 librime `data/minimal` 纳入产品数据。运行时不新增任何网络、遥测、更新或内容日志。

## 2. 本轮文件清单

以下仅列本轮实际新增或再次修改的路径；前轮 hardening 的完整清单保留在 `release-readiness-hardening.md`，不把全部脏工作树重复归功于本轮。

### 许可、决策、门禁与构建

- `AGENTS.md`
- `LICENSE`
- `README.md`
- `CMakeLists.txt`
- `data/product/NOTICE.txt`
- `data/manifests/license-decision.json`
- `data/manifests/language-readiness.json`
- `data/manifests/product-dependencies.json`
- `data/manifests/product-package-files.inc`
- `docs/product/license-decision.md`
- `docs/product/release-gate.md`
- `tools/product/build_identity.ps1`
- `tools/product/build_product.ps1`
- `tools/product/stage_product.ps1`
- `tools/product/check_release.ps1`
- `tools/product/source_contract.ps1`
- `tools/product/stage_source.ps1`
- `tools/build_librime_baseline.ps1`
- `tools/test_english_integration.ps1`
- `tests/product_source/source_producer_tests.ps1`
- `platforms/windows/product/tests/release_gate_tests.ps1`

### ABI、语言包及学习存储

- `engine/include/epocanva_engine.h`
- `engine/core/CMakeLists.txt`
- `engine/core/engine_api.cc`
- `engine/adapter/rime_adapter.h`
- `engine/adapter/rime_adapter.cc`
- `tests/engine_abi/CMakeLists.txt`
- `tests/engine_abi/assert_current_rime.cmake`
- `tests/engine_abi/c_consumer.c`
- `tests/engine_abi/contract_tests.cc`
- `engine/language_packages/CMakeLists.txt`
- `engine/language_packages/language_package.h`
- `engine/language_packages/language_package.cc`
- `engine/language_packages/module_requirements.h`
- `tests/language_packages/CMakeLists.txt`
- `tests/language_packages/main.cc`
- `engine/data_store/CMakeLists.txt`
- `engine/data_store/learning_store.h`
- `engine/data_store/learning_store.cc`
- `engine/data_store/windows_learning_repository.h`
- `engine/data_store/windows_learning_repository.cc`
- `tests/learning_core/CMakeLists.txt`
- `tests/learning_core/main.cc`
- `tests/learning_repository/CMakeLists.txt`
- `tests/learning_repository/assert_exit.cmake`
- `tests/learning_repository/main.cc`

### 英文核心及实际 Rime 集成

- `engine/languages/en/CMakeLists.txt`
- `engine/languages/en/language_support.h`
- `engine/languages/en/language_support.cc`
- `engine/languages/en/morphology_rules.h`
- `engine/languages/en/morphology_rules.cc`
- `engine/rime_components/english/english_translator.h`
- `engine/rime_components/english/english_translator.cc`
- `engine/rime_components/english/english_extended_config.h`
- `tests/english_core/CMakeLists.txt`
- `tests/english_core/language_support_tests.cc`
- `tests/fixtures/english_extended/default.yaml`
- `tests/fixtures/english_extended/epocanva_extended_english.schema.yaml`
- `tests/fixtures/english_extended/lexicon.tsv`
- `tests/fixtures/english_extended/frequency.tsv`
- `tools/rime_english_recovery/CMakeLists.txt`
- `tools/rime_english_recovery/main.cc`
- `tools/rime_english_recovery/recovery_test_support.h`
- `tools/rime_english_recovery/recovery_test_support.cc`
- `tools/rime_english_recovery/extended_tests.h`
- `tools/rime_english_recovery/extended_tests.cc`

### 中、法、日、韩独立核心

- `engine/languages/zh/phonetic_core.h`
- `engine/languages/zh/phonetic_core.cc`
- `tests/chinese_core/CMakeLists.txt`
- `tests/chinese_core/main.cc`
- `engine/languages/fr/french_core.h`
- `engine/languages/fr/french_core.cc`
- `tests/french_core/CMakeLists.txt`
- `tests/french_core/main.cc`
- `tests/french_core/assert_exit.cmake`
- `engine/languages/ja/romaji_composer.h`
- `engine/languages/ja/romaji_composer.cc`
- `engine/languages/ko/two_set_composer.h`
- `engine/languages/ko/two_set_composer.cc`
- `tests/composition_core/CMakeLists.txt`
- `tests/composition_core/main.cc`
- `tests/composition_core/test_support.h`
- `tests/composition_core/japanese_tests.cc`
- `tests/composition_core/korean_tests.cc`
- `tests/composition_core/invariant_tests.cc`
- `tests/composition_core/assert_result.cmake`

### Windows 产品整合

- `platforms/windows/engine_bridge/CMakeLists.txt`
- `platforms/windows/engine_bridge/rime_runtime.h`
- `platforms/windows/engine_bridge/rime_runtime.cc`
- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/product_manifest.h`
- `platforms/windows/product/product_manifest.cc`
- `platforms/windows/product/product_diagnostics.h`
- `platforms/windows/product/product_diagnostics.cc`
- `platforms/windows/product/product_settings.h`
- `platforms/windows/product/product_settings.cc`
- `platforms/windows/product/settings_window.h`
- `platforms/windows/product/settings_window.cc`
- `platforms/windows/product/ui_ids.h`
- `platforms/windows/product/tests/product_unit_tests.cc`
- `platforms/windows/product/tests/product_engine_tests.cc`
- `platforms/windows/product/tests/extended_settings_tests.cc`
- `platforms/windows/product/tests/runtime_modules_tests.cc`
- `platforms/windows/product/tests/ui_tests.cc`

### 边界、验证文档

- `docs/architecture/engine-abi.md`
- `docs/architecture/language-packages.md`
- `docs/testing/english-completeness.md`
- `docs/testing/learning-store.md`
- `docs/testing/learning-store-windows.md`
- `docs/testing/chinese-core.md`
- `docs/testing/french-core.md`
- `docs/testing/japanese-korean-core.md`
- `docs/reports/multilingual-foundation.md`

## 3. 实现选择及拒绝的替代方案

### 许可与无签名模式

`LICENSE` 为 GNU 官方完整文本，34,523 bytes，SHA-256 `0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0`。许可记录只承载用户实际决定，不制造公开发行、词表法域或签名批准。AGPL 原文应用指南中的示例措辞不覆盖项目明确的 version 3 only 授权。

格式 2 包清单将验收 profile、分发政策、路径例外分开。`internal-unsigned` 只允许不要求签名，不放宽干净源码、完整功能、隐私、对应源码、安装和宿主门禁。`package=ready` 只说明本地完整性，最终五语言验收状态由 `check_release.ps1` 给出。中央语言清单目前五项均保持 blocked，不通过修改布尔值虚构交付。

### ABI 和运行时

纯 C ABI 使用 size/version、不可复用 opaque token、caller-owned UTF-8 字节缓冲、代际/目标/快照验证；真实调用已有 RimeRuntime/RimeSession，未新增假的后端。容量不足不消费提交；已调用后异常禁止重放。TSF/executor 仍使用既有 C++ 桥，**本轮未宣称平台迁移完成**。

`required_modules` 保留英文默认，通过允许列表装载并校验实际模块；`default` 组负责 core/dict/gears，`levers` 仅用于明确部署。检查依据为实际固定 librime 的 `setup.cc`、`module.cc`、`rime_api_impl.h`，不是对任意外部插件的开放。既有 finalize/锁生命周期修复不回退。

### 英文与学习

英文核心区分基础语料计数、已学习计数和上一词 bigram。Alt12 weight 仍是源词典收录次数，不当作频率。DP 分段与下一词预测缺真实频率时明确 unavailable。测试合成数据不进入产品载荷。

形态生成是有界拼写规则，不是完整 POS/语义模型。大词典实测发现 exact lemma 的词形被前缀候选挤出菜单，已加可测排序偏好；产品默认过滤无词典收录的生成词形，所有格也要求实际基词已收录。缩写、收缩、二编辑拼写均为可关闭的显式候选，不自动覆盖用户原文。设置壳新增三个可回读开关，产品引擎测试覆盖二编辑独立 ON/OFF 对照。

学习核心只接收词典允许的 ID 和计数，最多 2,048 单词记录、2,048 bigram、8 词样本、262,144 UTF-8 字节文件。Unknown/Sensitive/off 写入拒绝，迁移原子完成，导出/删除需明确用户操作。Windows 实现当前用户 DACL、跨进程稳定锁、同卷句柄原子替换及多次动态权限检查；删除不是安全擦除，不控制宿主或系统备份。

**学习尚未接入实际输入路径。** 当前 Rime 公共候选接口仅有 text/comment，稳定词条 ID 未传到 TSF。宿主成功编辑与有效 `TfEditCookie` 下的实时权限锁必须同时覆盖持久化，不能在 `ResolveEditResult` 之后凭缓存权限调用存储。未以延迟队列或普通提交日志绕过此边界。

### 其他语言

中文实现拼音/注音语法和组合，日文实现 Romaji/Kana，韩文实现两套式 Hangul，法文实现显式重音和受限词形/候选接口。它们都是实际算法，**不是产品默认 schema**。未用固定汉字/Kanji候选或 OpenCC 日文字形配置冒充输入法。字典查询仅接受调用方数据且默认 unavailable。

法文的组合归一化/组合簇范围受限，不冒充完整 NFC/UAX #29；英文仍为既有 ASCII casing，未完成 Unicode folding、句首/标点/emoji完整性。其他语言词典及词频的具体选择/获取仍需授权；本轮未擅自引入。

## 4. 实际命令、退出码及失败保留

下列 `<repo>` 为仓库根；实际绝对参数、工具路径及进程退出码保存在构建证据 JSON/日志，不将机器路径写成通用配置。命令均通过 Git Bash 的 `powershell.exe` 包装运行。

```text
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/build_librime_baseline.ps1 -Mode english-completion -Fresh -Verbose_Build
```

首个 Fresh 构建由主线在编译中主动停止以整理源树，无成功退出码，不计通过。

```text
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/build_librime_baseline.ps1 -Mode english-completion -Verbose_Build
```

第二次真实 configure/build/install 成功，但脚本 exit 1：预期9个组件对象、实际11个。补齐 `morphology_rules.obj` 与 `language_support.obj` 清单后，同命令再次执行，最终 exit 0，导出与基线同为571项，路径/静态CRT检查通过。失败日志保留在 `build/librime-english/log-history/` 与 `build/multi-validation/`。

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 -EnginePrefix <repo>/build/librime-english/rime/install -BaselinePrefix <repo>/build/librime-baseline/rime/install -BuildDir <repo>/build/multi-english-off -LogTag extended-r1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 -EnginePrefix <repo>/build/librime-english/rime/install -BaselinePrefix <repo>/build/librime-baseline/rime/install -BuildDir <repo>/build/multi-english-off -PilotLexicon <repo>/build/english-pilot/epocanva_en_pilot.tsv -LogTag extended-pilot-r1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 -EnginePrefix <repo>/build/librime-english/rime/install -BaselinePrefix <repo>/build/librime-baseline/rime/install -BuildDir <repo>/build/multi-english-off -LogTag extended-off-final
```

三个入口实际 exit 0：39/39、45/45、39/39。执行了实际列表/JUnit名称比对，未过滤或跳过新增测试。源/API/缺词典/故意失败控制均保留。

```text
cmake -S . -B build/multi-core-aggregate -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE=<installed VS>
cmake --build build/multi-core-aggregate --config Release --parallel 4
ctest --test-dir build/multi-core-aggregate -C Release --output-on-failure --no-tests=error --output-junit aggregate-release.xml
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/product/build_product.ps1 -BuildDir <repo>/build/multi-product
```

核心 configure/build/CTest 均 exit 0，55通过/1跳过。产品首次176项运行未含52项核心测试且验证期间源码变动，构建入口 exit 9，无可打包收据。把顶层 `enable_testing()` 提前并加必需测试组存在性检查后，从稳定源码重跑：228注册、226通过、2跳过，入口 exit 0。

产品实际命令记录：`build/multi-product/verification-20261102T052820310Z-099a197d/commands.json`；同目录保留 configure/build/CTest日志、实际注册清单和JUnit。

```text
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File build/multi-validation/package_current.ps1
```

exit 0，包装器记录真实 `stage_product.ps1 -BuildDir ... -AcceptanceProfile internal-unsigned -DistributionPolicy internal-only` 与 `EpoCanva.exe --diagnose <BuildDir>/diagnose.txt --user-data-root <BuildDir>/diagnose-user`。不存在安装器/host/owner/signing伪造记录。四个TSF导出及三个二进制 `/dependents` 探测各 exit 0；原始结果在 `build/multi-validation/*-dependents.txt`、`product-exports.txt`。

其他实际失败：曾把PowerShell表达式交给Git Bash（exit127/2）；曾用 `vswhere -latest` 选到缺CMake的Community实例，外层PowerShell返回0但无构建执行，明确不计通过；改用已安装Professional并验证工具存在。英文核心 Debug 既有25ms性能阈值曾观测33.617ms而失败，未放宽，Release对应测试通过。归档器PS5 `File.Replace` 空备份参数问题已由真实负向套件发现并修正。源码归档原先反复遍历OneDrive祖先过慢，改为每阶段独立目录缓存，仍逐叶检查/哈希和最终重枚举。

独立代理运行的精确命令与失败历史分见各 `docs/testing/` 文档。它们的单元结果不替代本次正式产品收据。

## 5. 最终身份与测试边界

产品版本保留 `0.2.6-w3b`；版本号不代表已发行。

| 项目 | 值 |
| --- | --- |
| 产品源码身份 | `5c0f11b1fa94c965e1a00f920272860c2d20ff73+dirty.5564f29a49b9` |
| 产品源码 SHA-256 | `5564f29a49b95d25aca28025cc5ef64a4a63245f353748afcd730bdc67e49340` |
| 引擎源码 SHA-256 | `d8797770e657df43df33ca04a64d6dd87f1fee7b10a2dfdb6733c0a4e7ba4ed9` |
| EpoCanva.exe SHA-256 | `b9c2f57b9d21e38178ac5bc2ae9d37a7c7297571842e6630f09fecbea155a1fc` |
| epocanva_tsf.dll SHA-256 | `599252381d720eaf2a35a61a3a1d45723c04777cb0d43ec4e43154432ceb6608` |
| rime.dll SHA-256 | `15650540880774c356fc8fa0a0c0ef5701c7d5b33154c21c7156d521de977c97` |
| package.manifest SHA-256 | `44088f3c7e4e1b74051838b08c1812a2a9c8ce970dfdff1c68d4087c59004570` |
| 产品收据 SHA-256 | `1e57af0c9a8d5b802ea6f08a4a6da126e007536cdd2d3e15d22f89103c74effd` |

暂存位置：`build/multi-product/stage/EpoCanva-0.2.6-w3b`。这是未安装的英文扩展内部工程包，不是五语言候选交付物。

两项跳过：

1. `windows.tsf_context_observer.real_focus`：`SetFocus` 成功且 `GetFocus` 变化，但线程未收到 `OnSetFocus` sink通知。重验证分支测试通过不等于通知测试通过。
2. `learning.repository.symlinks`：`CreateSymbolicLinkW` 返回 Win32 1314，测试 exit77。没有提权或修改开发者模式；junction/hard-link与其他路径检查已经执行。

独立核心结果不累加成虚增产品测试数：ABI 9项、英文核心8项、中文8项、法文9项、日韩组合5项、学习核心8项、仓库11项、语言包3项已被纳入228项范围或对应核心聚合；各内部断言也不能代替真实宿主。

## 6. 未完成项及交付阻塞

- 英文生产语料频率与上下文/学习实际输入链路尚未完成；目前 `RankingInputs.allow_learning=false`。
- 英文 Unicode casing、句首/标点/emoji策略、真实跨应用一致性未完成。
- C ABI 已有真实后端，但 TSF/controller/coordinator尚未迁移；候选类型/词条ID元数据仍需独立安全通道。
- 中文Hanzi/整句、法文词汇驱动能力、日文Kanji数据未选用/获取/清权；没有把上游样例字典偷偷纳入包。
- 韩文组合已实现，但未接Rime/TSF/profile与产品词典候选；其他非英文也未进入默认schema。
- 组合 commit+live preedit 的Windows编辑协议仍拒绝，不以丢弃一半数据绕过。
- 安装器工具链、当前包安装/升级/回滚/卸载/残留及实际宿主候选可见性尚无完整证据；未执行新系统注册或UAC。
- 两项环境跳过保留；源码未提交，干净源码门禁不满足。未获提交授权，不通过擅自commit清理状态。
- 本地源码ZIP即使字节校验通过，也不自动证明AGPL的全部法定义务、源码取得可用性或从无Git元数据归档完整重建已验证。

Windows普通进程内TSF DLL没有OS网络沙箱；本轮的无网络为架构及依赖策略，静态导入表不等于证明整个宿主不联网。未来Android可通过不申请 `android.permission.INTERNET` 得到平台强制离线；本轮没有Android构建。

## 7. 实际源码归档及门禁

真实执行 `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/product/stage_source.ps1 -BuildDir <repo>/build/multi-product`，exit 0。归档含 17,649 个实际文件，34,373,729 bytes；SHA-256 `8418d30c66b2b9bd169f27f8ff7e63372788cfe9c54cadf34e12cc04489cda71`。位置 `build/multi-product/corresponding-source.zip`，对应 `source-manifest.json`。生产器已验证精确源码集合、当前源文件哈希、ZIP解压字节及前后稳定性；不代表已验证无Git元数据的完整重建。

源码清单为 5,473,074 bytes，SHA-256 `e73109f0e2b0cc5f33ba4cad47d647df0c38a73a7688d85aabe5477d26fdae5b`；归档内未压缩字节总数175,192,328。实际 ZIP 不包含 `.git`、用户数据或未清权 `data/minimal` 产品词典。

`build/multi-validation/check_current.ps1` 只把实际源码清单引用附加到真实诊断证据，然后依次调用两个真实 checker。包装器 exit 0 表示它成功记录了检查结果；**两个 checker 子进程均实际 exit 1**，不是验收通过：

```text
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/product/check_release.ps1 -BuildDir <repo>/build/multi-product -AcceptanceProfile public-signed
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/product/check_release.ps1 -BuildDir <repo>/build/multi-product -AcceptanceProfile internal-unsigned
```

- `internal-unsigned`：27项检查中13项通过、13项blocked、1项签名not_required；其中10项是当前profile技术阻塞，3项是公开分发阻塞。`corresponding-source` 独立打开 ZIP、哈希全部17,649项，并与当前文件、完整契约、包/收据核对，**passed**；真实隔离诊断、许可选择、身份及输入稳定性同样passed。签名为`not_required`，不是verified。
- `public-signed`：18项blocked。直接对内部包检查时，profile和分发范围不符，内部profile证据不得复用于公开profile；该拒绝不是源码ZIP损坏，而是证据范围不匹配。
- 最终两个profile均为 `technical_ready=false`、`acceptance_ready=false`、`distribution_authorized=false`、`signature_verified=false`、`release_ready=false`。

当前内部profile的10项技术阻塞：必需测试有跳过、源码dirty、五语言未完成并打包、无安装器、无安装器构建工具、无安装生命周期证据、无当前包真实宿主证据、完整英文能力验收未齐、完整offline/privacy验收记录未齐、五语言聚合验收未齐。静态导入检查和存储隐私单测不冒充完整offline/privacy验收。

最终报告：`build/multi-product/release-readiness-internal-unsigned.json`、`release-readiness-public-signed.json`；默认`release-readiness.json`保留内部profile结果。实际命令/时间/exit记录在 `build/multi-validation/gate-commands-20261102T055057860Z.json`（以实际生成文件为准）。最终重新执行 `Assert-ProductBuildReceipt`，仍与当前源码身份一致；`git diff --check` exit0，仅既有行尾转换警告。

**本轮可交接的是已验证工程代码、英文扩展内部暂存包和对应源码归档，不是满足用户聚合要求的五语言发行版。没有安装、发布、签名或提交。**
