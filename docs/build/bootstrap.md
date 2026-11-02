# 从零到可构建（bootstrap）

本文给出在本机 Windows x64 + Visual Studio 环境下配置、构建并运行 EpoCanva 测试的步骤，
以及**已实测确认**的阻塞与偏差。所有步骤不联网；工具的安装与 PATH 处理由开发者本人完成，
仓库内不含也不调用任何在线安装器。

采集方式说明：下文每条"可用/不可用"结论都来自本机真实命令输出，不是推断。
未验证的条目会显式标注**未验证**。

## 0. 前置条件与本机实测状态

| 工具 | 项目要求 | 本机实测 |
| --- | --- | --- |
| CMake | `CMakeLists.txt` 要求 ≥ 3.25；预设 schema v6 | **3.31.6-msvc6**，位于 VS 2022 Professional 内置目录；**不在普通 shell 的 PATH 中**（注册表 Machine/User PATH 均无 CMake 条目，实测） |
| ctest | 随 CMake | **3.31.6-msvc6**，同目录 |
| Ninja | 可选 | **1.12.1**，同 VS 内置目录；预设未使用它 |
| MSVC | 支持 C++17 | 三个实例：VS2022 **Professional**（工具集 14.44.35207）、VS2022 Community（14.44，其 `vcvars64.bat` 在本机失败）、VS 2026 BuildTools（14.50.35717）。CMake 的 VS 17 2022 生成器实际选中 **Professional 的 14.44** |
| MSBuild | VS 生成器需要 | 三处均存在 `MSBuild\Current\Bin\MSBuild.exe`；本次构建由 MSBuild 17.14.10 执行 |
| Windows SDK | — | 10.0.26100.0（CMake 配置日志确认已选用） |
| Git | — | 2.52.0.windows.1 |

获取 `cmake` 的推荐方式（**不修改系统环境配置**）：在
**Developer Command Prompt for VS 2022** 或 `Common7\Tools\VsDevCmd.bat -arch=amd64` 启动的会话里，
`cmake`、`ctest`、`ninja` 会同时进入 PATH。实测输出：

```
C:\Program Files\Microsoft Visual Studio\2022\Professional\...\CMake\CMake\bin\cmake.exe
cmake version 3.31.6-msvc6
```

## 1. 已验证开发路径与可选路径

**已验证并作为本项目当前开发路径**：Visual Studio 2022 Professional + CMake 3.31.6-msvc6
+ 生成器 `Visual Studio 17 2022` + `x64` + `Debug`（P0.1 运行验收已由架构负责人接受）。

| 预设名 | 生成器 | 本机能否配置 | 定位 |
| --- | --- | --- | --- |
| `vs2022-x64-debug` | `Visual Studio 17 2022` | **能**（configure/build/ctest 全部 exit 0） | **已验证开发路径** |
| `vs2026-x64-debug` | `Visual Studio 18 2026` | **不能**（exit 1） | **可选路径，本机未验证**；需要更新的 CMake，保留它是为了升级后不必改仓库 |

关于版本门槛的两件事实必须分开陈述：

- **上游文档确认**：CMake 官方发布说明 `Help/release/4.2` 原文为
  "The Visual Studio 18 2026 generator was added."
  （来源：https://cmake.org/cmake/help/latest/release/4.2.html）
- **本机实测**：可用 CMake 最高为 3.31.6-msvc6，其 `cmake --help` 生成器列表不含该生成器，
  `cmake --preset vs2026-x64-debug` 返回 exit 1 与
  `Could not create named generator Visual Studio 18 2026`。
  本机**没有**安装 4.2 做正向验证，因此"4.2 起可用"是引用上游文档，不是本机观测。

### 1.1 已验证路径的实际执行

```sh
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure --no-tests=error
```

实际结果（退出码均为观测值）：

| 步骤 | 退出码 | 关键输出 |
| --- | --- | --- |
| configure | 0 | `Selecting Windows SDK version 10.0.26100.0`、`The CXX compiler identification is MSVC 19.44.35209.0`、`C++ standard : 17 (extensions: OFF)`、`Configuring done (13.5s)` |
| build | 0 | `main.cc` → `build\vs2022-x64-debug\tools\toolchain_probe\Debug\epocanva_toolchain_probe.exe`，0 条警告 |
| `ctest -N` | 0 | `Total Tests: 4` |
| test | 0 | `100% tests passed, 0 tests failed out of 4` |

### 1.2 可选路径（升级到 CMake ≥ 4.2 之后才可尝试）

```sh
cmake --preset vs2026-x64-debug
cmake --build --preset vs2026-x64-debug
ctest --preset vs2026-x64-debug --output-on-failure --no-tests=error
```

构建目录 `build/vs2026-x64-debug` 与 `build/vs2022-x64-debug`、`build/manual-cl` 完全隔离。
**不得**在已生成的缓存目录里用 `-G` 强换生成器：CMake 会拒绝并保留旧缓存，
需要删除该构建目录后重新配置（删除动作由开发者自己执行，本仓库不自动清理）。

## 2. 手工 cl 编译（历史记录，非验收路径）

P0.1 首轮在 CMake 缺失时用过这条路径，产物在 `build/manual-cl/`。要点：

- 用的是 **VS 2026 BuildTools 的 14.50** 工具集，与 CMake 路径选中的 14.44 **不是同一个编译器**。
- 命令行未给 `/MD`，而 CMake 的 Debug 配置实际使用 **`/MDd`**（在真实构建日志中确认）。
  因此两者的导入表不同：手工产物只依赖 `KERNEL32.dll`，
  CMake 产物依赖 `MSVCP140D.dll / VCRUNTIME140D.dll / VCRUNTIME140_1D.dll / ucrtbased.dll / KERNEL32.dll`。
  **不要把任一导入表结论套到另一条路径上。**
- 这条路径不能替代 CTest，也没有运行 CTest。它留下的唯一价值是：
  在缺构建驱动时确认探针源码本身能编译、能按设计返回各级退出码。

## 3. 预设与构建规范

- 严格编译开关集中在根 `CMakeLists.txt` 的 `epocanva_strict` INTERFACE 目标；
  探针目标通过 `target_link_libraries(... PRIVATE epocanva_strict)` 继承。
- **传播已用真实构建日志核验**（不是假设）：MSBuild 详细输出里的实际 cl 命令行为

  ```
  CL.exe /c /Zi /nologo /W4 /WX- /Od /Ob0
         /D _CRT_SECURE_NO_WARNINGS /D NOMINMAX /D WIN32_LEAN_AND_MEAN
         /Zc:preprocessor /EHsc /RTC1 /MDd /GS /fp:precise
         /Zc:wchar_t /Zc:forScope /Zc:inline /std:c++17 /permissive-
         /utf-8 /Zc:__cplusplus /Gw  ...main.cc
  ```

  即 `/std:c++17`、`/permissive-`、`/utf-8`、`/Zc:__cplusplus`、`/Zc:preprocessor`、
  `/W4`、`/EHsc`、`/Gw` 与三个宏定义**确实到达编译器**。
- 关于"关闭语言扩展"必须说清楚：`CMAKE_CXX_EXTENSIONS OFF` 只对 GCC/Clang 产生
  `-std=c++17` 与 `-std=gnu++17` 的差别；**MSVC 没有 GNU 扩展模式**，
  上面的命令行里看不到任何扩展开关，MSVC 侧的严格性实际由 `/std:c++17` + `/permissive-` 提供。
- 共享的 `CMakePresets.json` 里**没有**本机绝对路径、没有 `-T` 工具集版本号（用默认工具集）。
  若将来确实需要锁定某个 VS 实例，写进 **`CMakeUserPresets.json`**（已被 `.gitignore` 排除），
  并在本节补一句原因；不要把实例路径塞进共享预设。
- **test 预设不会自动构建。** CMake 自带手册 `cmake-presets(7)` 列出的 test preset 字段中
  **没有** `buildPresets`；上一版预设写了该字段，被 CMake 以
  `Invalid extra field "buildPresets" in Preset` 拒绝（exit 1）。
  所以顺序必须是 configure → build → test。
- `.gitignore` 中的构建排除**已锚定为 `/build/`**。此前写的 `build/` 会连带匹配 `docs/build/`，
  导致 `docs/build/bootstrap.md` 被 Git 忽略（`git check-ignore` 实测命中 `.gitignore:2`）。
  改锚定后 `git check-ignore docs/build/bootstrap.md` 无输出，`/build/` 仍被排除。

## 4. 已知阻塞

1. **可选路径 `vs2026-x64-debug` 在本机不可配置**：CMake 3.31.6 不含 `Visual Studio 18 2026` 生成器，
   `cmake --preset vs2026-x64-debug` → exit 1，输出 `CMake Error: Could not create named generator Visual Studio 18 2026`。
   这**不阻塞本项目开发**：已验证路径是 `vs2022-x64-debug`（见第 1 节）。
   P1 起不再为该可选路径升级工具链；需要时由开发者本人升级到 CMake ≥ 4.2
   （上游发布说明确认该版本加入此生成器），再跑 §1.2 并回填真实退出码。
2. **cmake 不在常规 PATH**：只有进入 VS 开发者环境才能直接敲 `cmake`。
   这是使用体验问题，不是构建问题；改系统 PATH 属于修改机器配置，需开发者自行决定。
3. **VS2022 Community 的 `vcvars64.bat` 在本机失败**（errorlevel 1，
   `The system cannot find the path specified.`）。按本轮任务书要求**不再继续排查**，
   因为它不阻塞 CMake/MSBuild 路径（CMake 选中的是 Professional）。
4. **`/MDd` 带来的运行库依赖**：Debug 构建产物依赖 debug CRT（`ucrtbased.dll` 等），
   这些 DLL 只随 Windows SDK 分发，**不可再分发**。发布形态（Release `/MD` 或静态 CRT、
   VCRedist 打包）属于后续安装器阶段的问题，此处仅登记。

## 5. 第三方依赖

P1.1 起依赖已按 `third_party/sources.lock.json` 锁定并取回：
librime 1.17.0（`33e78140…`）检出在 `third_party/librime/`（被 `.gitignore` 排除），
其四个子模块按父仓库记录的提交初始化，Boost 1.89.0 只取头树并解压到
`build/librime-baseline/dependencies/boost/`，**不放进 `third_party/`**。
取回步骤与禁止事项见 `third_party/README.md` 第 3 节。

### 补丁登记表

| 编号 | 依赖 | commit | 目的 | 影响面 | 撤销方式 |
| --- | --- | --- | --- | --- | --- |
| — | — | — | 暂无补丁：上游检出到 P1.4 为止仍逐字节干净 | — | — |

规则见 `third_party/README.md`：上游检出保持字节干净，改动只以
`third_party/patches/NNNN-<dep>-<purpose>.patch` 表达，且每条补丁都要在此表登记。

### 项目侧构建适配输入（不是补丁，也不是上游能力）

这些文件不改上游一个字节，但**没有它们就复现不出本仓库的产物**，
因此与依赖同级别地登记在 `sources.lock.json` 的
`epocanva_build_adaptation_inputs`：

| 文件 | 注入点 | 用于 | 为何需要 |
| --- | --- | --- | --- |
| `tools/cmake/opencc_external_marisa_includes.cmake` | `CMAKE_PROJECT_opencc_INCLUDE` | `-Mode baseline` | OpenCC 只给内置 marisa 加 include/link，外部 marisa 分支缺这两步（`docs/build/librime-windows-baseline.md` §10.1） |
| `tools/cmake/epocanva_rime_compiled_in_components.cmake` | `CMAKE_PROJECT_rime_INCLUDE` | `-Mode component-probe`、`-Mode english-completion` | 上游插件机制不接受树外源码路径：`plugins/CMakeLists.txt:29-30` 会把 `ENV{RIME_PLUGINS}` 的目录前缀剥掉并把空格变成分号，`:33-39` 只 glob `plugins/*`，官方样例 `sample/CMakeLists.txt:36` 写的是"link or copy the directory to `plugins/`"。该适配用 `add_subdirectory` + `cmake_language(DEFER CALL …)` 在 `add_subdirectory(src)` 之后把自有 objects 交给 `rime` 目标，不新建上游目录、不放开外部 DLL 加载。P2.1 起名字来自 `EPOCANVA_RIME_COMPONENT_NAMES`，每个组件必须导出 `epocanva_rime_<name>_object_targets`，没导出就 FATAL_ERROR；ATTACHED 行带 mode 专属 label，于是一套模式的日志不可能为另一套模式作证 |
| `tools/build_librime_baseline.ps1` | — | 三种模式 | 选项集合、依赖顺序与全部后置断言；`-Mode` 默认 `baseline`，因此默认构建不会悄悄混入自有组件；自 P2.1 起脚本还会主动断言 baseline 的 rime CMakeCache 里没有任何 `^EPOCANVA_`/`CMAKE_PROJECT_rime_INCLUDE` 条目 |

## 6. 发行准备构建

`tools/cmake/epocanva_release_paths.cmake` 通过 `CMAKE_PROJECT_INCLUDE` 注入每个独立依赖工程与
librime。MSVC 19.44 的本机探针确认 `/pathmap` 需要同时启用 `/experimental:deterministic`；
映射目标为稳定的 `EpoCanva` 相对根，PE 中的 PDB 引用采用固定文件名。PDB 不进入产品包。
这属于项目侧构建输入，上游检出保持不变，不通过替换二进制字符串清理路径。
OpenCC 的 `PKGDATADIR` 原先展开为开发机安装前缀，`/pathmap` 无法改变普通宏字符串；
现有 OpenCC hook 在配置结束时移除 `PKGDATADIR`/`LOCALEDIR` 定义。固定源码
`src/Common.hpp` 在宏缺省时使用空 package fallback，`src/Config.cpp` 仍接受显式数据路径；
依赖数据生成工具由构建规则给定路径。产品英文 schema 不使用 OpenCC 转换数据。

依赖构建成功后扫描最终 DLL 的窄字符串与 UTF-16 字符串，拒绝个人源码路径。English 模式在
开始时作废旧 `engine-build-receipt.json`，成功后比较源码输入指纹并记录 DLL SHA-256；产品
构建和暂存必须同时核对引擎、产品收据。历史编号日志移动至各构建根的 `log-history/`，
不静默覆盖旧证据。

产品入口每次用 `cmake --fresh` 重建缓存，拒绝编译器环境覆盖，固定 trace OFF；
`-SkipTests` 只生成 `build_only` 状态，不能打包。正式构建比较实际 CTest 注册名集合和
JUnit 的执行名集合，将已声明的跳过项单独记录。内部候选可保留跳过事实；公开发行检查
要求必要测试实际执行。源身份、cache、测试清单、测试结果与三份二进制哈希均写入收据。
公开发行条件见 `docs/product/release-gate.md`，包完整性与许可批准是分开的判据。

## 7. 故障排查

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| `cmake: command not found`（但 VS 已装） | VS 内置 CMake 不自动进常规 PATH | 从 VS 开发者命令提示符/开发者 PowerShell 启动 shell |
| `Could not create named generator Visual Studio 18 2026` | CMake 版本不认识该生成器 | 用 `vs2022-x64-debug`，或升级 CMake；`cmake --help` 可列出当前支持的生成器 |
| `Invalid extra field "buildPresets" in Preset` | test preset 不支持该字段 | 删除该字段；构建与测试分两步执行 |
| `Could not find any instance of Visual Studio` | 生成器与实际安装的 VS 版本不符 | `cmake --help` 查生成器名后再改预设，不写本机绝对路径 |
| CTest 报 `No tests were found!!!` | 配置时关了 `EPOCANVA_BUILD_TESTS`，或没先执行 build 步骤 | 默认选项重新配置，并按 configure→build→test 顺序 |
| CTest 找不到探针可执行文件 | 忘了给 `-C Debug`（VS 生成器是多配置的） | 用 `ctest --preset vs2022-x64-debug`，预设已含 `configuration: Debug` |
| 探针报 `FAIL digest-mismatch` | 指纹字符串或哈希实现被改动 | 这是**预期行为**：用独立工具重算并同步常量，说明改动是有意的 |
| 探针报 `FAIL utf8-*` | 源文件字节被换行/编码归一化破坏 | 检查 `.gitattributes` 与编辑器编码；探针内是硬编码转义，正常不受编辑器影响 |
| `docs/build/` 下文件不出现在 `git status` | 构建排除模式未锚定 | 确认 `.gitignore` 用 `/build/` 而不是 `build/` |
