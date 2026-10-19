# ADR 0004 — 自有 Rime 组件以"编译进引擎 DLL"的方式接入

## 状态

Accepted（接入形态定义）。本 ADR 由 P1.4 任务书第二节固定，不是待选方案：
自有组件与引擎处于**同一个 PE 模块**，前端仍然只看见公开 C API。
P1.4 落地的第一个组件是测试探针 `epocanva_probe_processor`，
它是**可达性证明**，不是英文能力。

## 背景

P1.3 结束时登记的阻塞项是：自定义 Processor 有三种可选接法，
各自的构建图与产物形态不同（P1.3 报告 §10 路线 A/B/C）。P1.4 必须先定一条，
否则每个后续组件都要重谈一次。

决定前先核实了固定提交（librime `1.17.0` / `33e78140`）的实际机制：

| 事实 | 出处 |
| --- | --- |
| 上游合并插件的唯一入口是 `add_subdirectory(${plugin})`，`${plugin}` 要么是 `ENV{RIME_PLUGINS}` 经正则剥掉目录前缀后的**裸名字**，要么是 `plugins/*` 的 glob 结果 | `third_party/librime/plugins/CMakeLists.txt:24-47` |
| 剥前缀正则是 `[^ ]*/(librime-)?`，随后 `string(REPLACE " " ";" …)` 把空格变成分号 | 同上 `:29-30` |
| 官方样例对此的表述就是"link or copy the directory to `plugins/`" | `third_party/librime/sample/CMakeLists.txt:36` |
| 插件的 objects 与 `rime` 目标同库时，靠 `add_library(rime ${rime_src})` 里的 `${rime_plugins_objs}`（`$<TARGET_OBJECTS:…>`） | `third_party/librime/src/CMakeLists.txt:29-35, 84` |
| 模块名进 `kDefaultModules` 只有一条路：`RIME_SETUP_EXTRA_MODULES` ← `rime_plugins_modules` ← 上述插件目录 | `third_party/librime/CMakeLists.txt:264-271`、`src/CMakeLists.txt:174-177`、`rime/setup.cc:25` |
| **公开 C API 的 `RimeTraits.modules` 就是模块加载列表**；提供时用列表，未提供时用 `kDefaultModules` | `src/rime_api.h:101`、`src/rime_api_impl.h:53-54, 109-110` |
| 模块自注册靠 `.CRT$XCU` 静态初始化，库装载即写入 `ModuleManager` | `src/rime_api.h:528-533, 557-571`、`src/rime/module.cc:13-15` |
| 组件对象 `new` 在注册方、`delete` 由持有 `an<>` 的引擎方执行 | `src/rime/component.h:37`、`src/rime/engine.cc:298-327` |

结论：**上游没有"接受外部项目源码路径"的现成入口**。要走官方合并插件机制，
就必须在 `third_party/librime/plugins/` 下真实存在一个目录（链接或复制），
那会破坏"上游检出工作树干净"这一永久约束（`AGENTS.md` §7）。
`ENV{RIME_PLUGINS}` 这条在本机 additionally 不可用：仓库路径含空格，
`:30` 的空格替换会把一个路径拆成两个列表项。

`ENABLE_EXTERNAL_PLUGINS=ON` 也不是出路：它唯一能提供的模块名是 `plugins`，
而该模块在初始化时用 Boost.DLL 遍历 `<目录>/rime-plugins/*.dll` 并逐个装载
（`plugins/plugins_module.cc:35-70, 122-125`），正是本阶段要禁止的"任意外部插件加载"。

## 决策

1. **自有组件源码编译、链接进引擎 DLL。** 组件与 `rime` 目标处于同一个 PE 模块，
   不产出独立插件 DLL，不产出第二份引擎。
2. **注册发生在该 DLL 内。** 用上游真实机制：`RIME_REGISTER_CUSTOM_MODULE` 让模块在
   库装载时自注册进 `ModuleManager`；模块的 `initialize()` 里用
   `Registry::instance().Register("<组件名>", new rime::Component<T>)` 注册处理器。
3. **模块由前端通过公开 C API 的 `RimeTraits.modules` 指名加载**，
   不改 `kDefaultModules`（那需要上游插件目录机制）。
   因此"哪些自有模块要装载"是前端的显式配置，不是隐式默认。
4. **前端（含测试程序、后续 TSF/Android 适配层）只使用公开 C API**：
   `rime_api.h` 中已安装的声明，以及模块 `get_api()` 返回的**定长 C 结构 + 函数指针表**。
   前端不得 `#include <rime/…>`、不得 `new`/`delete` 任何 rime 对象、
   不得接收需要自己释放的 STL 对象或堆内存。
5. **构建配置沿用已验证的一套**：Release / x64 / `Visual Studio 17 2022` /
   静态依赖 + 共享 `rime` + 静态 CRT（`/MT`，由
   `third_party/librime/cmake/cxx_flag_overrides.cmake` 的
   `CMAKE_CXX_FLAGS_RELEASE_INIT` 决定），不引入新的 CRT 组合。
6. **不在 EXE 内创建或注册 C++ Processor**，**不开放任意外部插件或脚本加载**，
   `ENABLE_EXTERNAL_PLUGINS` 保持 `OFF`。
7. **上游改动仍然只能通过 `third_party/patches/*.patch` 表达**；本 ADR 不授权就地改树。
   本轮接入完全落在项目侧 CMake 适配里，未产生补丁。

## /MT 的准确说法（纠正一处容易写错的结论）

`/MT` **不等于**"跨模块调用必然失败"。它意味着：

- 每个模块各自静态链接一份 CRT，因而**各自有一份堆**；
  在 A 模块 `new`、在 B 模块 `delete` 是未定义行为，跨模块传 `std::string`/
  `std::vector` 会跨到两份不同的 STL 静态状态里。
- 跨模块的**纯 C 接口调用、以及双方都在同一模块内完成分配/释放的对象**仍然可用。

本决策把风险归零的方式不是换 CRT，而是**取消跨模块所有权**：
组件与引擎同模块，`new` 与 `delete` 都发生在这个 DLL 内；
跨过 DLL 边界的只剩 C ABI 的定长结构与函数指针。

## 限制

1. **组件依赖的是 librime 的内部 C++ 接口**（`rime/registry.h`、`rime/component.h`、
   `rime/processor.h`、`rime/key_event.h`）。这些头文件在 `INSTALL_PRIVATE_HEADERS=OFF`
   下**不随产物安装**，组件只能在引擎的源码构建内编译。
   **这些内部接口不属于前端稳定 SDK**：升级固定提交时必须重新核对签名，
   且核对证据属于那一阶段的构建报告。
2. **自有模块不在 `kDefaultModules` 里**。任何前端忘了在 `RimeTraits.modules`
   列出模块名，组件就静默不装载（`rime/setup.cc:33-40` 对找不到的名字直接跳过），
   表现为"配置里写了组件但没有效果"。因此**每一项自有组件都必须有一条
   行为学断言**（P1.4 用诊断计数 + 专用键消费），不能只看 schema 是否部署成功。
3. **接入依赖一个项目侧 CMake 适配**（`tools/cmake/…`，经
   `CMAKE_PROJECT_rime_INCLUDE` 注入，用 `cmake_language(DEFER CALL …)` 在
   `add_subdirectory(src)` 之后把 objects 交给 `rime` 目标）。它是**可恢复输入**，
   已在 `third_party/sources.lock.json` 的 `epocanva_build_adaptation_inputs` 登记；
   只按上游 SHA 取源**不能**复现本产物。
4. **产物形态与原版 librime 发行件不再逐字节相同**，因此 baseline 与
   component-probe 两套构建必须分开目录、分开进程，并各自记录 DLL 的
   实际加载路径与 SHA-256。
   记录哈希的**唯一**用途是钉死"产生某个结果的那个二进制"，以及证明两个 DLL 不是同一个文件。
   它**不是可复现性校验和**：P1.4 的复现轮实测同一份源码重链一次，
   `rime.dll` 只差 28 个字节（PE `TimeDateStamp`、调试目录时间戳、16 字节 PDB GUID），
   因此哈希必然每次构建都变。判"组件有没有真的进这个 DLL"要用与时刻无关的量
   （object 计数、组件字符串命中、导出表差集、运行时诊断计数），
   见 `docs/testing/rime-component-probe.md` §9。
   若将来需要字节级可复现，必须先按 `AGENTS.md` §7 处理链接器时间戳来源。
5. 本决策不解决 `docs/build/librime-windows-baseline.md` §10.1 的 OpenCC 钩子问题，
   也不改动 P1.2 已验证的依赖构建参数。

## 备选方案与未采纳原因

| 方案 | 未采纳原因 |
| --- | --- |
| 独立插件 DLL（`BUILD_MERGED_PLUGINS=OFF`） | 组件对象的 `new` 在插件模块、`delete` 在引擎模块，`/MT` 下两套堆；且需要导出 C++ 符号、两侧 STL/ABI 完全一致；任务书明确禁止 |
| 在测试 EXE 里直接 `Registry().Register(...)` | 注册发生在 EXE，组件类不在引擎 DLL 里；把前端变成引擎的一部分，违反边界（ADR-0002） |
| `ENABLE_EXTERNAL_PLUGINS=ON` + 运行期扫 `rime-plugins/` | 开放任意 DLL 装载，违反本阶段安全边界；且引入 Boost.DLL 依赖 |
| 把插件目录复制/链接进 `third_party/librime/plugins/` | 破坏上游工作树干净（`AGENTS.md` §7），并使 P1.2/P1.3 的"porcelain 0 行"验证失效 |
| 打上游补丁把组件名编进 `kDefaultModules` | 本轮不需要（`RimeTraits.modules` 已是公开入口）；若将来要"前端零配置"，再按 §7 走补丁流程 |
| 全静态产品（`BUILD_SHARED_LIBS=OFF` + `rime-static`） | 改变产物形态、失去可替换的引擎 DLL，收益小于代价；任务书禁止擅自改产品形态 |

## 后果

**正面**

- 跨 DLL 的 new/delete 与 STL 边界问题在这一层**结构性消失**，不需要 CRT 妥协。
- 前端边界仍然是纯 C ABI，与 ADR-0002 一致；换引擎实现时只有这一层要改。
- 组件的可达性可以被**行为学**验证（初始化计数、构造计数、专用键消费、实例回收），
  而不是靠"编译通过"或"配置里写了名字"。

**负面 / 成本**

- 组件与引擎同生命周期：不能单独热替换，升级 librime 必须重新编译全部自有组件。
- 内部 C++ 接口的破坏性变更只能在构建期发现，因此**每次升级都要跑可达性测试**。
- 构建多一条项目侧 CMake 适配边；`cmake_language(DEFER …)` 是 CMake ≥ 3.19 特性，
  本仓库当前固定 3.31.6-msvc6，升级 CMake 前不得移除该下限说明。

## 重新评估条件

1. 需要**不重编译引擎**即可替换某个组件（热替换、按客户分发不同组件集）时，
   重新评估插件 DLL 路线，届时 CRT/堆所有权方案必须与本 ADR 一并推翻重写。
2. 若"前端零配置装载自有模块"成为产品要求（例如第三方发行版复用我们的引擎 DLL），
   则按 `AGENTS.md` §7 提交最小上游补丁，把模块名编入 `kDefaultModules`。
3. 上游提供真正的"外部插件源码路径"入口（不再要求在检出树里放目录）时，
   改用官方机制并删除项目侧 CMake 适配。
4. `rime::Registry`/`Component`/`Processor` 的签名在某个固定提交变更后无法原样接入时。
5. 平台适配层（TSF / Android）出现必须跨模块共享组件对象的设计需求时。
