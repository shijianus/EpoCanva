# TSF profile helper ownership / rollback 审计与修复

状态：`DEVELOPMENT_POLICY_VERIFIED`；源码冻结，未 commit。真实注册、安装、卸载、升级和 MSI 事务回滚均为 `NOT_RUN`。本子任务不作产品 GO 判断。

## 1. 范围与环境

- 工作目录 `D:/EpoGO`；基线 HEAD `3401bf40c8701032fcf08cb8b886f2d17b50a71e`。保留共享未提交修改，没有切分支、提交、安装工具或改变系统配置。
- Windows x64 build 26200，Windows SDK 10.0.26100.0，VS 2022 / MSVC 19.44.35209.0，CMake 3.31.6-msvc6，C++17、extensions OFF、`/permissive-`。生产 helper 静态 CRT；纯策略测试另启用 `/W4 /WX`。
- 构建、临时文件、原始命令、失败日志、官方接口正文和快照都在 `build/go-helper-20261125/`。测试运行的只有假接口测试 EXE；真实 helper EXE 只编译，从未执行。
- 初始仅授权 helper / 相邻策略和测试 / 本报告；后续明确授权扩展 helper 来源清单与 helper 自身的独立 CTest。未改产品、TSF 焦点、根工程或 backend 的 CMake / 测试。

## 2. 根因与修复

| ID / 等级 | 原始问题 | 最终行为与验证 |
| --- | --- | --- |
| HELPER-01 / High | `Unregister` 先 RemoveLanguageProfile / Unregister，再调用 DLL 导出；helper 没有前置完整 owner 检查 | `ReadScopeOwner`、`ReadRegistrationOwner` 同时验证 HKCU/HKLM 的产品 marker、绝对 DLL 路径和 Apartment；任一错误、不同所有者或双范围注册都拒绝。每次 mutation 再检查。假接口用例和移除前置检查的变异版本验证。 |
| HELPER-02 / High | `Register` 的接口获取、TSF Register、AddLanguageProfile 或读回失败均可能无条件调用 DllUnregisterServer，撤销原有安装 | 先取得接口和状态，再记录本次尝试。补偿只删除入口时不存在且本次尝试创建的 target / processor / COM。原有 COM 和 processor 保留。所有阶段“失败未写入”和“部分写入后失败”均测试。 |
| HELPER-03 / High | `RegistrationOwnerMatches` 在第一个匹配范围返回，遗漏另一个范围；固定缓冲区可将未终止 REG_SZ 直接当字符串 | 两个范围均读取；不接受无终止符、嵌入 NUL、奇数 byte 数、错误类型或超出缓冲区的字符串。注册表 key 使用 RAII。双范围逻辑有纯测试；Win32 适配层本轮仅编译和源码核验。 |
| HELPER-04 / High | `GetProfile` 失败被等同于 absent；失败补偿可能凭错误观察继续删除 | 用 EnumInputProcessorInfo / GetLanguageList / EnumLanguageProfiles 的完整枚举确定状态；只有正常枚举结束才接受 absent。任何读取错误 fail closed。模拟每个必要读回失败，均非零退出。 |
| HELPER-05 / High | 注册补偿没有逐级检查结果；卸载失败后凭 presence-only 快照重新 AddLanguageProfile，无法恢复 enabled / active / icon 等设置 | 注册补偿子项失败就停止，保留父项，exit 5。卸载不再伪称事务恢复；失败立即非零退出，保留可检查的实际部分状态，不重建错误设置。 |
| HELPER-06 / Medium | 卸载时才发现 DLL / 导出缺失，可发生 profile 已删但 COM 无法处理 | 首次 mutation 前加载绝对路径 DLL 并解析两个导出，持有模块至操作与补偿结束。限制 DLL 搜索目录并核对实际加载路径。真实 DLL 失败路径未执行。 |
| HELPER-07 / Medium | helper 编译输入新增策略头后可能不被来源收据覆盖 | 已把策略头、纯测试和 tests/helper/CMakeLists.txt 纳入 Get-InstallerSourcePaths('helper')。实际正常来源校验通过，篡改策略头引用散列的记录被拒绝。 |

实现入口：`tools/product/isolation/tip_profile_helper.cc` 的 ReadRegistryString / ReadScopeOwner / ProfileOperations / ChangeRegistration；纯策略在 `tip_profile_policy.h` 的 CombineOwners / Step / Rollback / Register / Unregister。

保留原有同路径 COM 修复能力：已有注册仍调用 DllRegisterServer，由 DLL 自身快照负责其内部修复事务；helper 绝不因为该导出或后续 TSF 阶段失败而删除原有 COM。已有 target 不重复 AddLanguageProfile。已有其他语言 profile 的注册不删除这些 profile；卸载遇到它们明确拒绝，不能将一个语言的清理扩大为整个 processor 的删除。

拒绝的替代方案：只移动一处 owner 检查（会遗漏双范围和回滚）、所有失败后 DllUnregisterServer（会删原有安装）、将所有 HRESULT 失败视作 absent（误删）、用 AddLanguageProfile 重造既有状态（快照不足）、将两个同路径范围都视作本次所有（DLL 导出会遍历两者）。

## 3. 修改路径（完整清单）

| 路径 | 本次改动 |
| --- | --- |
| `tools/product/isolation/tip_profile_helper.cc` | 所有权适配、严格 registry 字符串、枚举状态、模块及 COM/key 所有权、受限补偿接入、绝对路径与异常传播 |
| `tools/product/isolation/tip_profile_policy.h`（新增） | 不依赖 Windows 的状态、前置校验、注册与卸载策略、补偿 journal |
| `tools/product/isolation/tip_profile_policy_tests.cc`（新增） | 68 个纯假接口测试；可选显式失败控制 |
| `tools/installer/provenance.ps1` | 仅 helper 来源数组一行增加策略头、测试和测试 CMake；保留其他 agent 已有内容及 CRLF |
| `tools/installer/helper/CMakeLists.txt` | 在已有 helper 构建后接入 BUILD_TESTING / tests/helper；保留已有静态 CRT 等设置 |
| `tests/helper/CMakeLists.txt`（新增） | 独立纯策略 EXE 和 installer.helper.ownership_policy 注册；无 Windows 链接依赖 |
| `docs/reports/go-audit-helper-20261125.md`（新增） | 本报告 |

基线备份包括 `tip_profile_helper.before.cc`、`provenance.before.ps1`、`helper-CMakeLists.before.txt`；精确新增 patch、最终文件 bytes / SHA-256 和快照见本地证据目录。commit SHA 为 N/A，本子任务没有提交权限且没有执行 commit。

## 4. 实际验证

| 验证 | 注册 / 实际执行 | 通过 | 失败 | 跳过 | 实际退出码 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 最终 helper 独立 CTest | 1 / 1 | 1 | 0 | 0 | 0 |
| 该测试内纯策略 cases | 68 / 68 | 68 | 0 | 0 | 0 |
| 显式失败控制（额外一个故意失败 case） | 69 / 69 | 68 | 1 | 0 | 1 |
| 临时变异：允许补偿删除已有 COM | 68 / 68 | 64 | 4 | 0 | 1 |
| 临时变异：移除卸载所有权前置检查 | 68 / 68 | 63 | 5 | 0 | 1 |
| helper 来源清单正常 / 策略头散列篡改拒绝 | 2 检查 | 2 | 0 | 0 | 0 |

最终 standalone configure、Release build 和 CTest 均实际 exit 0。CTest 只有一个注册的聚合测试，不能将 68 个内部 case 冒称 68 个 CTest 注册项。最终原始 JUnit 为 `helper-final-full.junit.xml`，其 system-out 包含全部 68 个 case 的结果；注册清单为 `helper-inventory-*.log` 的原始 JSON。

所有执行命令的**完整实际 argv、工作目录、UTC 起止时间、退出码**逐项保存在 `commands.jsonl` 与 `*.command.json`；日志不做成功过滤。最终关键命令的参数如下，CMake/CTest 的实际解析绝对路径保存在该本机记录中，不硬编码到版本控制文件：

```text
cmake -S tools/installer/helper -B build/go-helper-20261125/helper -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build/go-helper-20261125/helper --config Release --parallel 2
ctest --test-dir build/go-helper-20261125/helper -C Release --show-only=json-v1
ctest --test-dir build/go-helper-20261125/helper -C Release --output-on-failure --test-output-size-passed 65536 --test-output-size-failed 65536 --output-junit D:/EpoGO/build/go-helper-20261125/helper-final-full.junit.xml
powershell.exe -NoProfile -NonInteractive -File build/go-helper-20261125/check_provenance.ps1
build/go-helper-20261125/native/Release/tip_profile_policy_tests.exe --force-failure
build/go-helper-20261125/mutants-native/Release/delete_existing_com.exe
build/go-helper-20261125/mutants-native/Release/remove_before_owner_check.exe
```

这些命令均由 `python -B build/go-helper-20261125/run.py <记录名称> <实际命令及参数>` 捕获。变异只存在 build 目录的测试副本，从未写回源码。最终产品完整回归归主 agent，此处没有重复执行或声称通过。

保留的失败历史：最初本地临时 CMake harness 的仓库相对层级写错，configure exit 1，修正后 exit 0；官方 REG_SZ 正文检查因 NOT 大写导致大小写敏感断言 exit 1，正文已经成功保存，随后对相同正文进行大小写无关核验 exit 0。最终证据检查还发现 CTest 默认将成功输出截到 1024 bytes，检查脚本 exit 1；原始 LastTest.log 完整，随后明确提高输出容量重新执行，保留旧 JUnit 并生成完整的新 JUnit。以上失败都不被抹去或包装为成功。

## 5. 实际核验来源

- `platforms/windows/tsf/registration.h:11` 定义自动选择已有 scope；`:128` 以后声明注销会处理两个 scope，且不是事务。
- `platforms/windows/tsf/registration.cc:778` 的 UnregisterTextService 遍历 machine / user；`:154` 的 InspectOwner 使用 marker 或 DLL 文件名后缀的宽松判断。helper 因此必须独立要求 marker **与**确切路径，不能依赖该 DLL 来拒绝另一安装路径。
- Windows SDK `msctf.h:8129` 起核对 Register / Unregister / AddLanguageProfile / RemoveLanguageProfile / 枚举接口；`:1039` 核对 TF_LANGUAGEPROFILE；`:8925` 核对 Next 的请求与返回 record 数。`winreg.h:969` 核对 RegQueryValueExW 的 byte 单位。
- Microsoft Learn 已实际下载 EnumInputProcessorInfo、GetLanguageList、Unregister、RemoveLanguageProfile、IEnumTfLanguageProfiles::Next 和 RegQueryValueExW 正文。URL、SHA-256 与保存路径在 `official-specs.json`。官方明确枚举对象需要 Release、语言数组需要 CoTaskMemFree、S_FALSE 为枚举结束、REG_SZ 不保证 NUL 终止。
- Context7 返回的泛用枚举示例不适合据此推断本接口；最终依据上述实际官方正文和本机 SDK，而非泛用示例。

## 6. 集成与真实环境边界

1. **已纳入来源清单。** 最终候选必须重新构建 helper 和安装器收据；旧 helper/MSI/EXE 不包含本修复，不能复用旧产物身份。
   主链随后更新 common 来源以包含 `tools/product/evidence_contract.ps1` 并改用 `Read-ProductJson`；这些共享修改由主链所有，本任务不覆盖、不回退。上述来源正反测试记录的是其执行时的来源集合，最终候选仍须重新消费验证。
2. **已注册 helper 自身 CTest。** 当前 `build_helper.ps1` 原有调用只构建 `epocanva_tip_profile_helper` 目标，并不会自动运行 CTest。本子任务没有扩大权限改该脚本。主链使用该脚本的实际 CMake build 目录，再构建 `epocanva_tip_profile_policy_tests` 并执行 CTest，或者在该 helper 工程运行 build all。不能因 tests 已注册就声称脚本已执行测试。
3. **真实平台行为 NOT_RUN。** 未运行 helper 的任何命令，包括 status；未加载产品 DLL、调用 TSF 注册、写系统注册表、安装、升级或卸载。Win32 适配层的实际枚举、权限错误、模块加载、资源释放及多用户行为仍需获授权隔离宿主验证。
4. **并发边界。** 每一步检查可观察的 owner/scope/状态变化，但 TSF API 和现有 DLL 导出没有跨注册表/TSF 原子事务。其他进程在最终检查与写入之间改写注册表、同一 identity 的 ABA 替换、进程崩溃后的恢复，未被本假接口测试证明安全。正式安装生命周期必须串行且在可恢复隔离环境验证；不得宣称系统级原子性。
5. **卸载不伪称回滚。** 如果 RemoveLanguageProfile 成功而下一步失败，保留实际部分状态并返回非零，不恢复一个缺少 enabled/active/icon 信息的假 profile。需要完整 MSI/系统事务恢复时，属于后续真实生命周期验收。
6. **保守拒绝。** 双范围注册、未知所有权、COM 缺失但残留 TSF processor、额外同 CLSID 语言 profile、无法完整枚举，都要求显式检查，不自动清理。未来多语言安装器需要明确扩大所有权清单，不能移除此保护。

最短接入顺序：本范围源码冻结 → 主链候选身份与 helper 来源重新计算 → helper / MSI / EXE 重建及来源消费 → helper CTest 证据绑定 → 获授权隔离宿主的重复安装、修复、升级失败、卸载失败和恢复验证。无需等待真实宿主授权即可完成前四步。
