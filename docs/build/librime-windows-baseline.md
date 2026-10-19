# librime 最小 Windows 构建方案（P1.1 提案 → P1.2 已执行）

适用：EpoCanva 的第一个 librime 可构建基线。
所有开关名称、取值与位置都来自固定提交 `33e78140…` 的真实文件；
每条判断都标了证据行号；无法从源码确定的写 **未验证**。

> **本文状态**：§1–§9 是 P1.1 依据源码写的提案。P1.2 于 2026-10-17 真实执行了它，
> 执行结果、被推翻的条目与新的实测边界写在 §10.1、§12、§13，
> 逐条命令与退出码在 `docs/reports/P1.2-native-baseline.md`。
> §11 保留 P1.1 当时的"未做"声明，它是历史记录，不是当前状态。

## 1. 前提

| 前提 | 内容 | 依据 |
| --- | --- | --- |
| 工具链 | VS 2022 Professional + CMake 3.31.6-msvc6 + 生成器 `Visual Studio 17 2022` + `x64` | 本项目已验证开发路径（`docs/build/bootstrap.md` 第 1 节） |
| 不修改上游 | 本方案只用命令行与缓存变量，不碰 `third_party/librime/**` 一个字节 | `third_party/README.md` 第 5 节 |
| 不自动联网 | 根工程 CMake 配置阶段不得取源；所有取源是开发期显式动作 | `third_party/README.md` 第 1 节 |
| 不引入插件 | `RIME_PLUGINS` 环境变量不设 → 无插件参与 | `plugins/CMakeLists.txt:24-25` |

## 2. 推荐方案（一句话）

**依赖与 librime 全部在项目外构建目录中进行，安装到项目内被忽略的前缀，
依赖取静态库形态，CRT 取静态（`/MT`、Debug 为 `/MTd`），librime 产出单个 `rime.dll`；
第一轮基线关闭日志（`ENABLE_LOGGING=OFF`）与测试（`BUILD_TEST=OFF`）。**

为什么是这一条路（而不是别的）：

- 上游 Windows 脚本 `build.bat` 本身就是"静态依赖 + 静态 CRT + 共享 rime"的组合
  （`build.bat:108` 设 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`，
  `build.bat:111` 对依赖统一 `-DBUILD_SHARED_LIBS:BOOL=OFF`，`build.bat:181` 对 librime 设 `-DBUILD_STATIC=ON`）。
  因此这条路线**不需要给上游打补丁**。
- 输入法组件要注入任意宿主进程。**本轮把 `/MT` 当作一次构建实验的选择**，理由是可验证：
  产物不依赖 VC 运行库 DLL，`dumpbin /dependents` 能直接判定开关是否真的生效。
  ⚠️ 这不等于"静态 CRT 自动解决了跨模块运行库问题"——见 §6.1。
  `/MT` 也不是本项目已决定的发行策略；发行形态（`/MT` 或 `/MD`+VCRedist）
  要等 rime-adapter 与 TSF 层的宿主注入实验之后再出 ADR。
- 上游脚本会往**它自己的源码树**里装产物（`build.bat:79-80` 默认前缀 `%RIME_ROOT%`，
  `CMakeLists.txt:167` 又硬编码 `link_directories(${PROJECT_SOURCE_DIR}/lib)`），
  这与"上游字节干净"冲突；本方案把安装前缀指到项目内忽略目录，
  并靠 `-DCMAKE_PREFIX_PATH`（`build.bat:185` 同样做法）让各 `cmake/Find*.cmake` 找到产物。
- `Find*.cmake` 全部是 `find_path`/`find_library` 形式（例：`cmake/FindGlog.cmake:3,12`、
  `cmake/FindYamlCpp.cmake:3,20`），**不会**在配置阶段做下载，也不依赖 `add_subdirectory` 打进依赖，
  所以"先装依赖、再配 librime"的顺序是可行的。

## 3. 目录布局（全在已忽略的 `/build/` 下）

```
third_party/librime/            上游只读检出（.gitignore:44 精确排除）
build/librime/deps/<name>/      各依赖的构建目录（--fresh 生成）
build/librime/prefix/           依赖安装前缀：include/ lib/ bin/
build/librime/boost/            Boost 源码与 stage 产物
build/librime/rime/             librime 构建目录
build/librime/install/          librime 安装产物（rime.dll、头、data）
```

约束：**禁止**在 `third_party/` 内产生构建目录或安装产物；
每次运行时 `cmake -S ... -B build/librime/...` 的源与二进制目录必须分离。

## 4. 依赖构建顺序与实际开关

先决条件：子模块按 lock 记录的提交取（`git submodule update --depth 1 deps/<path>`），**禁止 `--remote`**。

| 顺序 | 依赖 | 固定提交 / 版本 | 本项目要传的开关（实际名称=取值） | 定义位置与作用 |
| --- | --- | --- | --- | --- |
| 1 | Boost | 1.89.0（官方目录实测存在；tar.gz SHA-256 `9de758db…c93`，`.7z` SHA-256 `d8b0c1d5…d49d`，元数据 commit `ef7fea34…`） | `toolset=msvc address-model=64 link=static runtime-link=static variant=release --with-regex`（另需 `--with-chrono --with-thread --with-atomic --with-filesystem` 与否：**未验证**，取 b2 报缺库再补） | 不是 CMake 工程，用 `bootstrap.bat` + `b2`；`env.bat.template:6` 的默认 BOOST_ROOT 即 1.89.0；README-windows.md:9 的下限 1.83 亦满足 |
| 2 | yaml-cpp | `2f86d137…` = 0.8.0 | `-DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_MSVC_SHARED_RT=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON` | 后两项取自 `build.bat:138-142`（上游 Windows 脚本用的值）；作用是产出静态、不共享 CRT 的 yaml-cpp，避免与 `/MT` 冲突 |
| 3 | leveldb | `99b3c03b…` = 1.23.0 | `-DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF -DBUILD_SHARED_LIBS=OFF` | `build.bat:128-129`；同时使 `third_party/googletest`、`third_party/benchmark` 两个嵌套子模块无需获取（`deps.mk` 同值） |
| 4 | marisa-trie | `3e87d53b…` = 0.3.1 | `-DENABLE_TOOLS=OFF -DBUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON` | `build.bat:160-162`；`ENABLE_TOOLS` 默认 ON（其 `CMakeLists.txt:9`）关成 OFF 后不建命令行工具；注意其 `cmake_minimum_required(VERSION 3.22)` |
| 5 | OpenCC | `556ed224…` = 1.1.9 | `-DBUILD_TESTING=OFF -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF -DBUILD_PYTHON=OFF -DBUILD_DOCUMENTATION=OFF`，其余保持默认 | `build.bat:171`；OpenCC 自身 `CMakeLists.txt:26-38`。**默认值会带出 3 个内置嵌套依赖**：`USE_SYSTEM_MARISA=OFF` → `add_subdirectory(deps/marisa-0.2.6)`（`opencc CMakeLists.txt:209-211`）、`USE_SYSTEM_DARTS=OFF`（头文件）、`USE_SYSTEM_RAPIDJSON/TCLAP=OFF` |
| 6 | （可选）glog | `7b134a5c…` = 0.7.1 | 第一轮不构建。将来构建时用 `-DWITH_GFLAGS=OFF -DBUILD_TESTING=OFF` | `build.bat:118-119`；关 gflags 后 gflags 依赖消失 |
| 7 | （仅测试）googletest | `f8d7d77c…` = 1.14.0 | 第一轮不构建；`-DBUILD_GMOCK=OFF` | `build.bat:151` |

`--target install` 一律加 `-DCMAKE_INSTALL_PREFIX=<repo>/build/librime/prefix`（`build.bat:112` 用的是同一开关，只是值不同）。

### 4.1 OpenCC 的构建期 Python 依赖

`opencc/data/CMakeLists.txt:4-5` 用 `${PYTHON_EXECUTABLE}` 调 `scripts/merge.py`、`scripts/reverse.py`
生成繁简词典，`README-windows.md:14` 也写明需要 Python。
这些步骤由 `add_custom_target`（`data/CMakeLists.txt:39`）承载，是否属于默认 `ALL` **未验证**。
基线要求：第一轮**不构建词典**，若实测 `--target install` 触发了 Python 生成步骤，
则改为只构建并安装库目标（例如 `--target cc` 之类，实际目标名需配置后从目标列表确认，**未验证**），
**不得**为此引入 Python。

## 5. librime 本体的开关取值（全部 14 个 option 都列，含不动的）

定义位置均为 `third_party/librime/CMakeLists.txt`，行号如后。

| 开关 | 基线取值 | 定义行 | 作用与理由 |
| --- | --- | --- | --- |
| `BUILD_SHARED_LIBS` | `ON` | :18 | 产出 `rime.dll`。EpoCanva 的 rime-adapter 与引擎同进程内使用；先按上游主形态做，避免同时验证两种形态 |
| `BUILD_MERGED_PLUGINS` | `ON`（默认，不动） | :19 | 无插件时该值无实际影响；保留默认以免引入上游未验证组合 |
| `BUILD_STATIC` | `ON` | :20 | "依赖作为静态库"。`build.bat:181` 同值；与 §4 的 `-DBUILD_SHARED_LIBS=OFF` 依赖产物一致，并驱动 `Boost_USE_STATIC_LIBS`、`Glog_STATIC`、`YamlCpp_STATIC` 等（:52-58） |
| `BUILD_DATA` | `OFF`（默认） | :21 | 不构建/安装词库数据；本阶段禁止引入词库产物 |
| `BUILD_SAMPLE` | `OFF`（默认） | :22 | 不建示例插件 |
| `BUILD_TEST` | `OFF`（**注意与上游默认相反**） | :23 | 上游默认 ON 且 `find_package(GTest REQUIRED)`（:118-119）。基线先关掉以去掉 gtest；第二步再打开做上游单测回归 |
| `BUILD_SEPARATE_LIBS` | `OFF`（默认） | :24 | 单库形态；开启会拆出 `rime-dict`/`rime-gears`/`rime-levers`（`src/CMakeLists.txt:102-142`），增加链接面 |
| `ENABLE_LOGGING` | `OFF`（**与上游默认相反**） | :25 | 去掉 glog/gflags 两个依赖（§依赖审查 5.2 的源码级证据），并符合"日志不得含用户内容"的隐私取向（`docs/adr/0003`） |
| `ALSO_LOG_TO_STDERR` | 不适用 | :26 | 仅在 `ENABLE_LOGGING=ON` 有意义（`RIME_ALSO_LOG_TO_STDERR`，:103-105） |
| `ENABLE_ASAN` | `OFF` | :27 | 且其实现只写 Unix 风格标志（:43-50），MSVC 上不可用 |
| `INSTALL_PRIVATE_HEADERS` | `OFF`（默认） | :28 | 基线不对外提供插件头文件 |
| `ENABLE_EXTERNAL_PLUGINS` | `OFF`（默认） | :29 | 与"不引入插件"一致；开启会额外 `dl` 依赖（`src/CMakeLists.txt:41-43`） |
| `ENABLE_THREADING` | `ON`（默认） | :30 | 关掉会定义 `RIME_NO_THREADING`（:110-112），改变部署器行为；基线保持默认 |
| `ENABLE_TIMESTAMP` | `ON`（默认） | :31 | 关掉定义 `RIME_NO_TIMESTAMP`（:114-116）。**注意**：可复现构建角度我们更想要 OFF，但那会改变词库产物签名行为，属未验证组合 → 基线保持 ON，改动留给独立实验 |

其余要显式传入的：

| 变量 | 取值 | 依据 |
| --- | --- | --- |
| `CMAKE_GENERATOR` | `Visual Studio 17 2022` + `-A x64` | 覆盖 `env.bat.template:9-12` 的 `ARCH=Win32` / `VS2019` / `v142` 默认值 |
| `CMAKE_PREFIX_PATH` | `<repo>/build/librime/prefix` | `build.bat:185`；各 `cmake/Find*.cmake` 靠它定位 |
| `CMAKE_INSTALL_PREFIX` | `<repo>/build/librime/install` | 覆盖 `build.bat:80` 的 `%RIME_ROOT%\dist`（会写脏上游） |
| `CMAKE_CONFIGURATION_TYPES` | `Release`（或 `Debug`） | `build.bat:103` |
| `CMAKE_MSVC_RUNTIME_LIBRARY` | `MultiThreaded$<$<CONFIG:Debug>:Debug>` | `build.bat:108`；即 `/MT` 与 `/MTd` |
| `CMAKE_USER_MAKE_RULES_OVERRIDE(_CXX)` | `<repo>/build/librime/…/c(_cxx)_flag_overrides.cmake` | `build.bat:105-106`；这两个文件把 `CMAKE_CXX_FLAGS_DEBUG_INIT` 设为 `/MTd`、`RELEASE_INIT` 设为 `/MT`（见 `cmake/cxx_flag_overrides.cmake:3-6`） |
| `CMAKE_EXE_LINKER_FLAGS_INIT` | `-llibcmt` | `build.bat:107`（静态 CRT 需要 libcmt） |

⚠️ **两套 CRT 机制同时存在**：`CMAKE_MSVC_RUNTIME_LIBRARY`（CMake ≥ 3.15 的正式开关）与
`CMAKE_USER_MAKE_RULES_OVERRIDE` 改写 `*_FLAGS_INIT`（旧手法）都被上游传入。
在策略 `CMP0091=NEW` 下前者优先，但**这是推断，不是本机观测**。
基线第一步必须用真实编译命令行确认最终落到 `cl` 上的是 `/MT` 还是 `/MD`；
确认后**只保留一种机制**，并把结论写回本文（见 §7 的实测项）。

## 6. 链接形态矩阵与一致性要求

| 目标 | 本方案形态 | 说明 |
| --- | --- | --- |
| librime (`rime` 目标，`src/CMakeLists.txt:84`) | **动态 DLL** | `BUILD_SHARED_LIBS=ON` |
| 依赖（yaml-cpp / leveldb / marisa / OpenCC / Boost） | **静态库** | `BUILD_STATIC=ON` + 各依赖 `-DBUILD_SHARED_LIBS=OFF` |
| CRT | **静态 `/MT`（Release）、`/MTd`（Debug）** | `build.bat:108` + `cmake/cxx_flag_overrides.cmake:3-6` |
| EpoCanva 自有目标（`epocanva_strict` 使用者） | **必须与上表同一种 CRT** | 见下 |

一致性规则（下一步构建配置必须照此执行）：

1. **凡是链接这批静态库的目标，一律同一 `CMAKE_MSVC_RUNTIME_LIBRARY` 取值。**
   静态库把自身所用 CRT 的对象留在 `.lib` 里；跨 `/MT` 与 `/MD` 混链会出现
   `operator new`/堆句柄/std::string 跨模块不一致的未定义行为。因此：
   我们的 `rime-adapter`、以及后续任何直接链接 `rime.lib`/依赖静态库的目标，
   都必须在**同一份缓存设置**下构建（写进 `vs2022-x64-debug` 预设的 `cacheVariables`，
   而不是散落在命令行里）。
2. 现有 P0.1 探针是 CMake 默认的 **`/MDd`**（真实命令行已在构建日志中确认），
   与上游默认 `/MT` 不同。目前二者互不链接，所以**尚无冲突**；
   一旦 rime-adapter 加入，就必须统一到 §5 的取值，并让基础设施测试也在同种 CRT 下构建。
3. Debug/Release 不可混用：`/MTd` 与 `/MT` 也是不同 CRT。
4. 依赖装到同一前缀后，**不要**在同一目录里混放 Debug 与 Release 产物；
   分前缀（`prefix-debug` / `prefix-release`）或分 `CMAKE_CONFIGURATION_TYPES` 单次构建，二选一，本方案选后者。

### 6.1 统一 CRT 不等于安全：跨 rime.dll 边界的内存所有权

`/MT` 只保证"堆与 C 运行库对象在同一份镜像里"。它**不**回答"谁分配、谁释放"这个问题，
而 `rime_api.h` 的接口里两者是分开约定的。以下为固定提交 `33e78140…` 的实测读码结论
（行号均指 `third_party/librime/src/rime_api_impl.h`，除非另注）：

| 接口 | 分配方 | 释放方 | 客户端禁止做的事 |
| --- | --- | --- | --- |
| `get_status` → `RimeStatus::schema_id`、`schema_name` | rime.dll 内 `new char[len+1]`（`:347-350`） | `free_status` 内 `delete[]`（`:364-365`） | 自行 `free()`/`delete[]`/改写；跨进程缓存该指针 |
| `get_context` → `composition.preedit`、`commit_text_preview`、`menu.candidates[].text/comment`、`menu.select_keys`、`select_labels[]` | rime.dll 内 `new char[]`（`:223`、`:231` 等） | `free_context` 内逐字段 `delete[]`（`:288-304`） | 同上；尤其不要把 `preedit` 交给宿主的字符串销毁函数 |
| `get_commit` → `RimeCommit::text` | rime.dll 内 `new char[]`（`:319-320`） | `free_commit` 内 `delete[]`（`:330`） | 同上 |
| `get_input`（`:1062`）、`get_version`、`get_shared_data_dir`（旧版裸指针形） | rime.dll 内部所有 | **没有** free 接口 | 不得 free；只读、不得跨 `finalize()` 保存 |
| `get_*_dir_s(char* dir, size_t buffer_size)` | **调用方**的缓冲区 | 调用方 | `buffer_size` 单位是 **UTF-8 码元（字节）**；实现是 `strncpy`（`rime_api.cc:49-73`），截断时**不保证补 `\0`**，调用方必须自己检查长度 |
| `RimeTraits` 的各个 `const char*` | 调用方 | 调用方 | 字符串必须活过 `setup()`/`initialize()` 返回之后；`SetupDeployer` 立刻转成 `path`/`std::string`（`setup.cc:42-66`），但 `app_name` 等仍按实现要求确认 |
| `RimeSessionId` | rime.dll 内 `Service` 持有 `an<Session>` | `destroy_session` 或 `finalize`（`StopService`→`CleanupAllSessions`，`service.cc:80-83`） | 不得当指针解引用；它只是 `reinterpret_cast<uintptr_t>(session.get())`（`service.cc:92`），生命周期由引擎管 |
| `CandidateList` / `RimeCandidateList` 迭代器 | rime.dll | `candidate_list_end` / `candidate_list_from_index` 配套释放 | 不得用 `free_context` 释放，也不得跨 `finalize()` 使用 |

三条对适配层的硬约束：

1. **不得跨边界释放**：凡是指针来自 `get_*`，只能交给配对的 `free_*`。
   本轮的生命周期程序严格按此写，且没有对任何上游对象调用 `delete`/`free`。
2. **`finalize()` 之后不得再用任何旧句柄**：`Registry::Clear()` 与
   `ModuleManager::UnloadModules()`（`:61-62`）会销毁组件与模块，
   此前取到的 `const char*` 全部失效。
3. **CRT 一致性仍然必须保持**：因为静态库把自身的 CRT 对象留在 `.lib` 里，
   混链 `/MT` 与 `/MD` 会在链接期就报 `LNK2005/LNK2038`
   （本轮实验确实制造过一次：以 `/MD` 编 OpenCC、链 `/MT` 的 `marisa.lib`，
   报 `libcpmt.lib(locale.obj) : error LNK2038: 检测到"RuntimeLibrary"的不匹配项:
   值"MT_StaticRelease"不匹配值"MD_DynamicRelease"`）。
   这条是"必须同一种 CRT"的实测依据，但它只覆盖 CRT 对象，**不覆盖上表的分配/释放约定**。

### 6.2 本轮实验的边界（不要推广）

- `/MT` 是**本轮构建实验**的取值，不是已决定的发行策略。
- 上述所有权规则来自读码与一次生命周期运行，**不**等于 rime-adapter 的 ABI 已设计完成。
- 生命周期测试通过**不**代表输入、候选、纠错能力通过（那三者本轮未实现）。

## 7. DLL 查找与产物布局（不依赖"碰巧存在的全局 DLL"）

采用 §5 的静态 CRT + 静态依赖组合后，运行期外部 DLL 依赖应当**只剩系统 DLL**
（`KERNEL32`、`ADVAPI32` 等）与可选的 ICU/iconv（见下"未验证"）。要求：

1. 交付布局固定为：
   ```
   build/librime/install/bin/rime.dll
   build/librime/install/lib/rime.lib        （导入库）
   build/librime/install/include/…           （rime_api.h 等，来自 install(FILES) 规则 CMakeLists.txt:231）
   ```
2. **验收必须用 `dumpbin /dependents` 实际列出依赖**，而不是假设：
   - 若出现 `MSVCP140.dll` / `VCRUNTIME140.dll` / `ucrtbased.dll` → 说明 §5 的静态 CRT 没生效，
     要么修正开关，要么显式走 `/MD` + 随包 VCRedist 路线（当前**不选**，理由见 §2）。
   - 若出现第三方 DLL（marisa/opencc/leveldb/yaml-cpp 的 dll）→ 说明依赖建成了动态，违反 §4。
3. `iconv` / `ICU`：`src/CMakeLists.txt:56-57` 引用 `${ICONV_LIBRARIES}` 与 `${ICU_LIBRARIES}`，
   而根 `CMakeLists.txt` 只在 `cmake/FindIconv.cmake` 里探测 iconv，**没有任何 `find_package(ICU)`**
   （全仓库对 ICU 的引用仅此一处变量）→ 两者在 MSVC 下预期为空。是否真的为空 **未验证**，
   需在配置日志中确认；若 OpenCC 要求 ICU，则 ICU 将成为新的必需依赖，必须回到本文重评。
4. 宿主进程内加载 `rime.dll` 的查找路径由后续 TSF 适配层负责显式化
   （`LoadLibraryEx` 全路径 / 与宿主 DLL 搜索顺序隔离），本轮不设计。

## 8. 计划执行的命令（本轮**不执行**；标注是否会联网）

以下命令在 VS 2022 Developer Command Prompt（x64）中执行，路径以 `<repo>` 代指仓库根。

```bat
rem ---- A. 取子模块（会联网：git 官方仓库；不加 --remote）----
cd <repo>\third_party\librime
git submodule update --depth 1 deps\yaml-cpp deps\leveldb deps\marisa-trie deps\opencc
git submodule status        rem 期望：全部已初始化，且 SHA 与 sources.lock.json 一致

rem ---- B. Boost（会联网：官方 archives.boost.io；先核对 SHA-256 再解压）----
rem 解压位置固定在 <repo>\build\librime\boost，不得放进 third_party\
cd <repo>\build\librime\boost\boost_1_89_0
rem tar.gz SHA-256 9de758db755e8330a01d995b0a24d09798048400ac25c03fc5ea9be364b13c93
certutil -hashfile <download>\boost_1_89_0.tar.gz SHA256
rem 构建：只到够用的库；缺哪个补哪个
bootstrap.bat
b2 toolset=msvc address-model=64 link=static runtime-link=static variant=release --with-regex stage
set BOOST_ROOT=<repo>\build\librime\boost\boost_1_89_0

rem ---- C. 依赖，逐个独立目录 + 统一前缀（不联网）----
cmake -S <repo>\third_party\librime\deps\yaml-cpp -B <repo>\build\librime\deps\yaml-cpp -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CONFIGURATION_TYPES=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
  -DBUILD_SHARED_LIBS=OFF -DYAML_MSVC_SHARED_RT=OFF -DYAML_CPP_BUILD_CONTRIB=OFF ^
  -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON ^
  -DCMAKE_INSTALL_PREFIX=<repo>\build\librime\prefix
cmake --build <repo>\build\librime\deps\yaml-cpp --config Release --target install
rem 同样方式依次处理 leveldb / marisa-trie / opencc（开关见 §4）

rem ---- D. librime（不联网）----
cmake -S <repo>\third_party\librime -B <repo>\build\librime\rime -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
  -DCMAKE_PREFIX_PATH=<repo>\build\librime\prefix ^
  -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC=ON -DBUILD_TEST=OFF -DBUILD_DATA=OFF ^
  -DBUILD_SAMPLE=OFF -DBUILD_SEPARATE_LIBS=OFF -DENABLE_LOGGING=OFF ^
  -DENABLE_EXTERNAL_PLUGINS=OFF -DINSTALL_PRIVATE_HEADERS=OFF ^
  -DCMAKE_INSTALL_PREFIX=<repo>\build\librime\install
cmake --build <repo>\build\librime\rime --config Release --target install

rem ---- E. 验收（不联网）----
dumpbin /dependents <repo>\build\librime\install\bin\rime.dll
git -C <repo>\third_party\librime status --porcelain     rem 期望：空（上游未被写脏）
```

联网汇总：**A（git 取子模块）**、**B（Boost 归档下载）**；C/D/E 必须全程离线。
若某步报错需要临时联网查资料，按 `AGENTS.md` 记录为开发期查询，不得把下载塞进构建脚本。

## 9. 基线验收信号（下一步用）

1. 每个依赖 configure 日志里 `Build files have been written to:` 全部指向 `<repo>/build/...`，
   无一处指向 `third_party/`。
2. ~~Boost.Regex 静态库产出，且命名符合 MSVC auto-link 预期（**未验证**…）~~
   —— **P1.2 已推翻此条**。`CMakeLists.txt:68` 的 Windows 分支不要求组件**不是上游缺口**：
   librime 用到的 Boost 组件在 1.89.0 全是头文件用法，`${Boost_LIBRARIES}` 展开为空是正确结果。
   源码证据见 `docs/compliance/dependency-audit.md` §5.4；本轮实测值：
   `Boost_FOUND=TRUE`、`Boost_INCLUDE_DIR=<repo>/build/librime-baseline/dependencies/boost/boost_1_89_0`、
   `Boost_LIBRARIES=''`，`rime.dll` 链接成功。
   **因此不要给 `:68` 补 `REQUIRED COMPONENTS regex`** —— 那会去要求一个上游并不产出的库。
3. `rime.dll` 成功产出且 `dumpbin /dependents` 不含第三方 DLL 与 VC 运行库。
4. 上游检出仍 `git status --porcelain` 为空。
5. 我们自己的 4 条基础设施测试在同一 CRT 设置下仍全绿。

## 10. 若必须改上游才能继续

**P1.1 的评估曾预备了一条 Boost 补丁；P1.2 判定它不需要、而且有害**（见 §9.2）。
本文原先写明的处理顺序仍然有效，只是本轮没有用到它：

1. 优先尝试 CMake 认可的缓存变量入口（不改源码）；
2. 仍不行则**停止该子步骤**，登记阻塞，给出最小补丁建议，
   补丁形式 `third_party/patches/NNNN-<name>-<purpose>.patch`，
   并在 `docs/build/bootstrap.md` 的补丁表登记目的与撤销方式；
3. 任何情况下都不就地修改上游树。

### 10.1 P1.2 实际用到的一条：OpenCC 的外部 marisa

P1.2 唯一"上游命令行不足"的地方是 OpenCC 的 `USE_SYSTEM_MARISA=ON` 分支：

- 它 `find_library(LIBMARISA NAMES marisa)`（`deps/opencc/CMakeLists.txt:213`）后
  **从不使用**这个变量，也不给外部 marisa 添加任何包含目录
  （`deps/opencc/src/CMakeLists.txt:6-8` 只在**内置**分支加 `../deps/marisa-0.2.6/include`）；
- `target_link_libraries(libopencc marisa)`（`src/CMakeLists.txt:118`）用的是**裸名字**，
  在内置分支里那是一个真实 target，在外部分支里只剩一个链接器要按搜索路径解析的 `marisa`。

**未打补丁的做法（本轮采用）**：用 CMake 官方支持的
`CMAKE_PROJECT_<projectName>_INCLUDE` 钩子注入一个自有文件
`tools/cmake/opencc_external_marisa_includes.cmake`，由它执行
`include_directories()` + `link_directories()`，并额外传
`-DCMAKE_CXX_STANDARD=17`（marisa 0.3.1 的公共头使用 `std::string_view` 与嵌套命名空间，
而 OpenCC 对 MSVC 分支不设置 C++ 标准，只给 GNU/Clang 写 `-std=c++14`，见其
`CMakeLists.txt:155-178`）。上游树因此保持字节干净。

> 曾试过的两条死路，记录下来避免重犯：
> ① `-DCMAKE_CXX_FLAGS:STRING=/I"<prefix>/include"`：CMake 的命令行解析会**剥掉引号**，
> 含空格的路径被拆成 `/ID:\Users\Smart` 与剩余片段，编译直接失败；
> ② 设置 `INCLUDE`/`LIB` 环境变量：CMake 生成的 `.vcxproj` 用
> `AdditionalIncludeDirectories`，**不读** `$(INCLUDE)`，实测头文件仍找不到。

**最小补丁建议（本轮不需要，仅登记）**：若要让 OpenCC 无需外部钩子，
把 `deps/opencc/src/CMakeLists.txt:6-8` 的条件反转为
"外部时也把 `LIBMARISA` 所在前缀的 `include` 加进来"，并把 `:118` 改为
`target_link_libraries(libopencc ${LIBMARISA})`；同时在其根 `CMakeLists.txt`
为 MSVC 设置 `CMAKE_CXX_STANDARD 17`。三条改动可合成一个补丁文件，
但在本轮**没有**被应用。

## 11. 本轮明确未做

未构建任何依赖、未构建 librime、未运行 `build.bat` / `install-boost.bat` / `install-plugins.sh`、
未下载 Boost、未初始化子模块、未引入插件、未引入词库、未改上游源码。
本文是**基于源码的提案**，其中标注"未验证"的条目必须由下一次真实构建来判定。

## 12. P1.2 实际执行的命令序列（可复现）

路径含空格，所以命令都必须在**带引号**的前提下执行；下文以 `<repo>` 代指仓库根，
执行者需自行替换。所有命令都只在 VS2022 Professional 的 x64 开发者环境里运行。

### 12.1 取源（开发期显式动作，见 §13）

```bat
rem 已在 P1.1 完成的父仓库克隆见 third_party/README.md 第 3 节
cd <repo>\third_party\librime
git submodule update --init --depth 1 deps\yaml-cpp deps\leveldb deps\marisa-trie deps\opencc
git submodule status
git status --porcelain
```

Boost 用 §13.2 的 curl + 校验 + 解压流程，**不运行 `b2`**。

### 12.2 构建依赖与 librime（离线）

```powershell
# 一次性全量：-Fresh 删除本轮自己创建的构建目录，-Verbose_Build 让真实 cl 命令行进日志
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command `
  "& '<repo>\tools\build_librime_baseline.ps1' -Verbose_Build -Fresh"
```

入口内部按 marisa → yaml-cpp → leveldb → opencc → rime 的固定顺序执行，
每一步都是 `cmake -S … -B …`（源与二进制目录严格分离）+ `cmake --build --config Release`
+ `cmake --install --config Release`，并逐一检查退出码。
19 个步骤的日志在 `build/librime-baseline/logs/NN-<stage>.log`。

### 12.3 生命周期程序与 CTest（离线）

```powershell
$cm = '<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ct = '<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'

# 独立工程，不 add_subdirectory 进根工程
& $cm -S '<repo>\tools\rime_lifecycle_smoke' -B '<repo>\build\librime-baseline\smoke' `
      -G 'Visual Studio 17 2022' -A x64 `
      -DCMAKE_CONFIGURATION_TYPES:STRING=Release `
      -DRime_Install_Prefix:PATH='<repo>\build\librime-baseline\rime\install'
& $cm --build '<repo>\build\librime-baseline\smoke' --config Release
& $ct --test-dir '<repo>\build\librime-baseline\smoke' -C Release `
      --output-on-failure --no-tests=error
```

### 12.4 原有四个基础测试仍按 P0.1 的预设执行（配置未改）

```sh
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure --no-tests=error
```

### 12.5 验收用的观测命令（全部只读）

```sh
git -C <repo>\third_party\librime status --porcelain          # 期望空
git -C <repo>\third_party\librime submodule status            # 4 行无 +/-，2 行带 '-'
dumpbin /headers <repo>\build\librime-baseline\rime\install\lib\rime.dll    # 期望 8664 machine (x64)
dumpbin /dependents <repo>\build\librime-baseline\rime\install\lib\rime.dll # 期望无 VC 运行库、无第三方 DLL
dumpbin /directives <repo>\build\librime-baseline\prefix\lib\marisa.lib     # 期望 RuntimeLibrary=MT_StaticRelease
```

## 13. 取源步骤（Acquisition，开发期动作，构建入口不联网）

`tools/build_librime_baseline.ps1` 默认**完全不联网**，也不安装任何软件。
它只核验下面这些已经取好的源码身份；任何一项不符就停止并报告，而不是自行取源。
取源命令由开发者本人显式执行，逐条对应 `third_party/README.md` 第 3 节。

### 13.1 librime 与其子模块

```sh
# 父仓库：按 tag 浅克隆，再比对完整 SHA
git clone --branch 1.17.0 --depth 1 https://github.com/rime/librime.git third_party/librime
git -C third_party/librime rev-parse HEAD     # 必须等于 lock 的 pinned_commit

# 只取本轮真正需要的 4 个；禁止 --remote（会漂到分支 HEAD，破坏 lock 语义）
git -C third_party/librime submodule update --init --depth 1 \
  deps/yaml-cpp deps/leveldb deps/marisa-trie deps/opencc

# 自检：4 个已初始化的 SHA 必须与 lock 的 recorded_commit 逐字相等，
# deps/glog 与 deps/googletest 必须仍是未初始化（每行以 '-' 开头）
git -C third_party/librime submodule status
git -C third_party/librime status --porcelain    # 必须无输出
```

`glog` / `googletest` **故意不取**：`ENABLE_LOGGING=OFF` 使
`find_package(Glog REQUIRED)`（`CMakeLists.txt:76-83`）整块不执行，
`BUILD_TEST=OFF` 使 `find_package(GTest REQUIRED)`（`:118-119`）不执行。
本轮构建结束后 `git submodule status` 仍是 6 行中 2 行带 `-`，
这是**预期状态**，不是漏取。

### 13.2 Boost（官方源码包 + 官方完整性信息）

```sh
# 1) 先取官方校验元数据（只读 GET）
curl -sS -L --proto "=https" -o boost_1_89_0.7z.json \
  https://archives.boost.io/release/1.89.0/source/boost_1_89_0.7z.json

# 2) 取官方源码包（.7z 与上游 install-boost.bat:14 用的是同一个对象）
curl -sS -L --proto "=https" -o boost_1_89_0.7z \
  https://archives.boost.io/release/1.89.0/source/boost_1_89_0.7z

# 3) 计算并逐字比较（两个独立工具互验）
sha256sum boost_1_89_0.7z
certutil -hashfile boost_1_89_0.7z SHA256
# 期望，完整值，不接受省略号：
# d8b0c1d593d68fe359616aa2fae4b3e258ca1f90ed57eb46c5494c29cdd1d49d

# 4) 解压到被忽略的构建树内，**不得**放进 third_party/
7z x boost_1_89_0.7z        # -> boost_1_89_0/
```

本轮实测：下载 123,506,971 字节，`sha256sum` 与 `certutil` 均为
`d8b0c1d593d68fe359616aa2fae4b3e258ca1f90ed57eb46c5494c29cdd1d49d`，
与官方 `boost_1_89_0.7z.json` 的 `sha256` 字段逐字相同；
元数据 `commit` 为 `ef7fea34711a189472893b88205b1dd3c275677b`，`created` 为 `2025-08-06T18:44:03Z`。
`boost/version.hpp:22` 的 `BOOST_VERSION` = `108900`。

**不运行 `b2`，不编译任何 Boost 库**（理由见 §9.2 与依赖审查 §5.4）。
上游的 `install-boost.bat` 也没有编译库：它 `:20` 只跑 `.\b2 headers`。
不使用 aria2c，也不执行任何上游脚本。

### 13.3 联网范围声明

本轮联网仅用于：按 tag/记录提交 `git clone`/`git submodule update --init` 取官方源码；
从 `archives.boost.io` 只读下载官方源码包与其校验元数据；查询官方发布页。
**未上传任何本地文件、仓库内容、机器信息或用户数据**；未使用镜像站或预编译包；
未运行 `build.bat`、`install-boost.bat`、`install-plugins.sh` 或任何上游脚本；
未安装系统软件；未提权；未修改全局 PATH 或系统配置。
