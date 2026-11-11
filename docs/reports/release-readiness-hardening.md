# 发行准备修复记录（2026-11-02）

> 历史阶段记录：本报告的许可证未定、161项测试和能力缺口描述对应当时的修复轮次。
> 随后用户已选择AGPL-3.0-only，新增ABI、多语言核心及228项聚合验证，当前状态见
> [`multilingual-foundation.md`](multilingual-foundation.md)。保留下面的原始验证范围，不追改历史结果。

## 1. 环境与范围

Windows 11 x64，MSVC 19.44.35209（VS 2022 Professional）、Windows SDK
10.0.26100.0、CMake/CTest 3.31.6-msvc6。工具通过现有 VS Developer Shell 使用，
未安装工具、修改系统 PATH 或提权。起始 HEAD 为
`5c0f11b1fa94c965e1a00f920272860c2d20ff73`。

本轮承接用户“根据审计进一步完善至交付发行”的请求。该请求扩展了旧 W3-B
任务书排除的构建可追溯、隐私修复和二进制路径清理范围；EpoCanva 自身许可证未定、
词表再分发决定、禁止擅自提交以及真实系统写入需具体授权等约束仍然有效。
本轮未决定许可证、未签名、未发布、未执行系统注册或安装/卸载。

开始时 `tools/product/isolation/layer_c/c_layer_agent.ps1` 与 `trigger.ps1`
已有未提交修改，且另一个会话继续修改/执行隔离验证。它们不是本轮产出，未覆盖。
本轮所有构建与验证产物均在 `build/<preset>/`。

## 2. 文件清单

本轮新增：

- `data/manifests/product-dependencies.json`
- `data/manifests/product-package-files.inc`
- `data/product/licenses/README.txt`
- `data/product/licenses/rapidjson-1.1.0-license.txt`
- `docs/product/release-gate.md`
- `docs/reports/release-readiness-hardening.md`
- `platforms/windows/product/tests/build_receipt_tests.ps1`
- `tests/windows/engine_bridge/runtime_fault_api.cc`
- `tests/windows/engine_bridge/runtime_fault_tests.cc`
- `tools/cmake/epocanva_release_paths.cmake`
- `tools/product/build_identity.ps1`
- `tools/product/check_release.ps1`

本轮修改：

- `README.md`
- `data/product/NOTICE.txt`
- `docs/build/bootstrap.md`
- `docs/compliance/dependency-audit.md`
- `docs/product/requirements.md`
- `docs/windows/input-permission-contract.md`
- `platforms/windows/engine_bridge/CMakeLists.txt`
- `platforms/windows/engine_bridge/rime_runtime.cc`
- `platforms/windows/engine_bridge/rime_runtime.h`
- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/product_diagnostics.cc`
- `platforms/windows/product/product_diagnostics.h`
- `platforms/windows/product/product_engine_bootstrap.cc`
- `platforms/windows/product/product_engine_bootstrap.h`
- `platforms/windows/product/product_engine_config.cc`
- `platforms/windows/product/product_engine_config.h`
- `platforms/windows/product/product_manifest.cc`
- `platforms/windows/product/product_manifest.h`
- `platforms/windows/product/tests/deployment_tests.cc`
- `platforms/windows/product/tests/product_unit_tests.cc`
- `platforms/windows/product/tests/registration_tests.cc`
- `platforms/windows/tsf/registration.cc`
- `platforms/windows/tsf/registration.h`
- `platforms/windows/tsf_context/permission_query.cc`
- `platforms/windows/tsf_context/permission_query.h`
- `platforms/windows/tsf_product/product_test_seam.cc`
- `platforms/windows/tsf_product/product_test_seam.h`
- `platforms/windows/tsf_product/text_service_controller.cc`
- `tests/windows/engine_bridge/CMakeLists.txt`
- `tests/windows/tsf_context/permission_tests.cc`
- `tests/windows/tsf_coordinator/input_permission_gate_tests.cc`
- `tests/windows/tsf_coordinator/real_engine_tests.cc`
- `tests/windows/tsf_edit/test_host.h`
- `tests/windows/tsf_product/product_sink_tests.cc`
- `tools/build_librime_baseline.ps1`
- `tools/cmake/opencc_external_marisa_includes.cmake`
- `tools/product/build_product.ps1`
- `tools/product/stage_product.ps1`

其他会话还新增/修改了 `docs/reports/W3-B-c-layer-sessions.md`、
`docs/windows/isolation-verification-plan.md` 和前述两份 `layer_c` 脚本；本轮未写这些文件，
没有将其中旧包证据转换为本包验收结果。

## 3. 实现选择

- 保留当前 `0.2.6-w3b` 内部 Alpha 版本及未获公开分发许可的声明。完整性检查与发行许可分开。
- 修复未限定的 `EngineBootstrapState`，按当前源码重新编译，不采用旧测试二进制。
- `InputScope::GetValue` 返回 `E_FAIL` 判为 `Unknown`。正常的 `S_OK/VT_EMPTY`
  仍可表示未声明范围；不从输入内容猜测 OTP。未明确发布 scope 且返回失败的宿主将关闭建议，
  普通文字仍归宿主。当前没有新增用户学习层。
- librime 清理异常保留终止失败状态和进程 runtime slot，避免重复 `finalize()`；
  同进程不能重启一个可能尚未清理的引擎。故障测试是与生产相同 MSVC/CRT 的测试 DLL，
  不代表上游通常抛异常，也不冒充真实 OS 卸载失败实验。
- 多个产品附件共享部署文件锁；首附件退出后其余附件仍保护引擎数据，最后租约停止后才释放。
  清理失败时保留文件锁至进程退出，但释放独立的启动 mutex。局部测试观察租约 2→1→0，
  在每个关键点由独立子进程申请部署锁，避免仅靠延时判断。
- 诊断错误字符串改用现有严格 UTF-8/UTF-16 转换器，避免宽字符直接窄化破坏非 ASCII 信息。
- 产品运行库 SHA-256 编译进产品 DLL。异步启动前对当前 `rime.dll` 核验，
  不匹配时拒绝引擎启动；它是防错装/替换的身份检查，不是防管理员篡改的信任根。
- 构建入口固定 trace OFF，验证实际 cache，并记录完整 HEAD、dirty 内容指纹、
  构建命令/真实退出码、JUnit、配置与三份二进制身份。失败/跳测不颁发可打包收据。
  产品源码指纹排除文档和独立 `tools/product/isolation/` 实验工具；真实宿主证据必须另绑工具哈希。
- 路径映射使用本机真实验证的 MSVC `/experimental:deterministic` + `/pathmap`，
  以及稳定的 PE/PDB 名称。OpenCC 的宏展开安装前缀另由项目侧 hook 去除，
  保留显式路径查找；没有改上游检出或二进制字节。
- 上游构建身份与当前自有 English 源码另行绑定，引擎收据避免只重编产品而复用过期 English DLL。
- 共享载荷表固定 25 个文件（14 个许可目录文件），由打包器与运行时使用同一规则。
  缺文件、删除 manifest 行、同时改文件和清单哈希、额外文件、来源身份不一致均拒绝。
  默认严格路径扫描；显式内部诊断例外不能报告 package ready。
- RapidJSON 随 OpenCC 进入 runtime，补其固定版本完整许可；TCLAP 仅用于未打包工具。
  补 X11 keysymdef 与 Alt12Dicts 原始声明，纠正 utfcpp 为 BSL-1.0。
  旧锁文件中的阶段 DLL/recipe 哈希是历史记录，不改写历史结果。
- 注册假后端改为 Win32 的非递归删除语义，按快照回滚新建类别父键；卸载保留外来父键内容。
- `check_release.ps1` 只读取包与证据，输出机器可读阻塞项。它不制造审批、验收结果，
  不触发注册/安装/真实输入。缺法律决定、安装器或绑定当前包的验收证据时非零退出。

## 4. 已执行命令

所有命令在仓库根执行。完整本机路径仅保存在 git-ignored 的 `build/` 证据中。

| 命令 | 观察到的退出码 | 结果 |
| --- | --- | --- |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build/release-validation/run_root.ps1` | 0 | 全新根工程配置/构建/CTest，各子步骤 0；8/8 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File platforms/windows/product/tests/build_receipt_tests.ps1 -RunRoot <repo>/build/release-gate-tests/receipt` | 0 | 当前收据接受；失败/过期/篡改收据拒绝 |
| 上一命令使用独立 `receipt-negative` 目录并加 `-ForceFailure` | 14 | 预期的可失败控制，不能算普通通过 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/build_product.ps1 -BuildDir <repo>/build/release-gate-tests/build-missing -RimeDll <repo>/build/release-gate-tests/absent.dll` | 3 | 缺运行库立即停止；收据为 in_progress，不能打包 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/build_product.ps1 -BuildDir <repo>/build/release-product -SkipTests` | 0 | 产品完整集成编译通过；明确 build_only，不能打包 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build/release-validation/run_targeted.ps1` | 0 | 初次产品编译后的 6 项身份/载荷/注册测试通过；后续改动须最终重跑 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File platforms/windows/product/tests/build_receipt_tests.ps1 -RunRoot <repo>/build/release-gate-tests/receipt-final` | 0 | 增加 cache 篡改拒绝后再验证 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build/release-validation/compile_product.ps1` | 0 | 含共享锁/路径映射的最终产品代码构建通过，尚未使用新 English DLL |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File build/release-validation/run_targeted.ps1 -Filter '^(product.unit.(settings_manifest_paths\|registration_fake_registry\|registration_can_fail\|deployment_cleanup_lock)\|product.build.\|product.integration.(sink_engine_identity\|sink_lease_lock))$' -Label final-targeted` | 0 | 6 项包/注册/锁/运行库身份用例通过；实际集合以 XML 为准 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build_librime_baseline.ps1 -Mode baseline` | 1 | 编译/安装 0，后置 CRT 日志证据不足；未进入 English 构建 |
| 同上加 `-Verbose_Build`（第二轮） | 1 | 静态 CRT 检查通过，最终路径扫描发现 OpenCC 默认数据目录，未进入 English 构建 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build_librime_baseline.ps1 -Mode baseline -Verbose_Build`（第三轮） | 0 | OpenCC 数据目录适配后，配置/构建/安装/静态 CRT/导入表/路径扫描通过 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build_librime_baseline.ps1 -Mode english-completion -Verbose_Build` | 0 | 真实重编全部引擎与自有组件，完成身份/导出/CRT/路径校验并签发引擎收据 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/build_product.ps1 -BuildDir <repo>/build/release-product` | 0 | 最终真实构建/CTest及收据生成；160 通过、1 跳过 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 -EnginePrefix <repo>/build/librime-english/rime/install -BaselinePrefix <repo>/build/librime-baseline/rime/install -BuildDir <repo>/build/release-english-off -LogTag 20261101T211831979Z` | 0 | 29/29 |
| 上一命令改用 `-BuildDir <repo>/build/release-english-on -PilotLexicon <repo>/build/english-pilot/epocanva_en_pilot.tsv` | 0 | 35/35 |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/stage_product.ps1 -BuildDir <repo>/build/release-product -StageRoot <repo>/build/product/stage/EpoCanva-0.2.6-w3b-hardened` | 0 | 严格暂存；未覆盖旧包 |
| `<stage>/EpoCanva.exe --diagnose <repo>/build/release-product/diagnose.txt --user-data-root <repo>/build/release-product/diagnose-user` | 0 | 无界面包诊断；写入限定 build 目录 |
| `dumpbin /exports <stage>/epocanva_tsf.dll` | 0 | 四个所需导出，精确集合验证通过 |
| `dumpbin /dependents` 对三个 staged binary 分别执行 | 各 0 | 依赖输出保存于 package-check |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/check_release.ps1 -BuildDir <repo>/build/release-product -StageRoot <stage>` | 1 | 真实发行条件阻塞，报告 12 项，未颁发发行就绪结论 |

完整本机命令原文与执行记录保存在 `build/release-validation/`、引擎 logs/log-history、
产品 verification 与 package-check 目录中。脚本实现使用参数数组，不让 shell 重新解释路径。

基线 `rime.dll` SHA-256 为
`fc855ff327fd66ba54acf0fb60781e0f087c44afab355ae0d2d36016dae63d71`；
English `rime.dll` SHA-256 为
`525ceea528cc273b413925b9317bcc550889fff78314f5e7ff4e68875b22a400`。
引擎源码输入指纹为
`7203883da427c245c691427fe9562d49a895017abb023881649b79148fd84bd2`。
这不是外部签名或发布批准。

## 5. 验证口径

已完成的最终产品验证：

| 项目 | 实际结果 |
| --- | --- |
| 根工程 Debug | 8/8 通过，exit 0 |
| 基线与 English 引擎重编 | 最终均 exit 0；详细失败迭代见 §4 |
| 当前产品 Release 配置/构建/全量 CTest | 子步骤均 exit 0；161 项，160 通过、0 失败、1 显式跳过 |
| 跳过项 | `windows.tsf_context_observer.real_focus`，未提供真实焦点正向证据 |
| 英文集成 pilot 关闭 | 29/29 通过，exit 0 |
| 英文集成 pilot 开启 | 35/35 通过，exit 0；与历史 33 条不同，按本次实际生成清单报告 |
| 严格暂存 | exit 0；25 个载荷文件，4,669,836 bytes，未使用路径例外开关 |
| 新包 `--diagnose` | exit 0；`package=ready`、`manifest=ok`、`manifest_failures=0`、`user_data=ok` |
| 产品 DLL 导出表 | dumpbin exit 0；恰有四个所需导出；测试 seam 未进入产品 DLL |
| 最终三 binary 依赖表 | dumpbin 均 exit 0；报告保存在 `build/release-product/package-check/` |
| 公开发行检查 | exit 1，`release_ready=false`，12 个明确阻塞；不是构建失败 |

`build/release-product/verification-20261101T211843870Z-0e7c7a3b/`
含精确参数数组、实际退出码、CTest 注册清单与 JUnit；
`build/release-product/product-build-receipt.json` 绑定当前源码、cache、测试结果和二进制。

候选路径为 `build/product/stage/EpoCanva-0.2.6-w3b-hardened/`。
源码身份：`5c0f11b1fa94c965e1a00f920272860c2d20ff73+dirty.ceacaac1974c`。
manifest SHA-256：`05f50b2e4adac34ca39dbc194c7b119e98da97004da74f1081fc48a5e2e7e8d4`。
产品 DLL SHA-256：`da5372740f0c9ef16b11dc03826340ab8eade3539dfbb6515349ee153c4beda3`。
设置 EXE SHA-256：`8610501e3539fce441d0f32e50b35135b87d1691274a07801a59b3a53e0be66c`。

`internal-only=0` 表示没有使用路径检查例外，不表示法律公开发行许可。
NOTICE 仍保留内部 Alpha 与许可未决声明，公开 gate 正确阻止发布。
诊断使用 `build/release-product/diagnose-user`，没有写默认用户数据根。
`release-evidence.json` 只含本轮实际执行的诊断记录，没有伪造安装、宿主或审批记录。

真实宿主实验正在独立会话中进行。此前证据曾证明旧包注册和清理成功，
但未开始有效输入或总体失败。不能继续宣称“从未注册”，也不能据此判定本轮新包验收通过。
任何实际注册状态都是时间点快照，不写为持久产品状态。

## 6. 未完成事项与风险

公开发行仍依赖：EpoCanva 许可选择及明确批准、词表目标法域再分发结论、
安装器工具与实际安装/升级/修复/回滚/卸载验收、当前包真实宿主输入闭环、签名与
发行证据。英文学习/形态/连写分段/缩写等核心能力仍未实现，自有跨平台 C ABI 与 Android
也不由本轮构建修复完成。

当前 Windows 输入字段元数据不能保证识别每个 OTP 字段；关闭读取失败路径并非
OS 提供了新的隐私保证。Windows 离线行为是代码和依赖的架构约束，普通 TSF DLL
不受 OS 网络沙箱隔离。Android 未来可通过不声明 `android.permission.INTERNET`
获得平台权限限制；当前没有 Android 产物。

## 7. 下一轮的真实阻塞

不以脚本输出 PASS 代替缺失能力。发行审批依据必须指向冻结包、源码、二进制、
工具及测试记录的准确哈希。未经用户要求，本轮不提交、不推送、不创建标签，
dirty 身份如实保留；待负责人作出发行相关决定后，才能冻结干净源码并完成最终验收。
