# 依赖与许可证审查（P1.1 提出，P1.2 更新）

对象：librime 固定提交 `33e78140250125871856cdc5b42ddc6a5fcd3cd4`（tag `1.17.0`）。
本文所有"证据"列都指向**该提交下检出树里的真实文件与行号**，或指向**在该 SHA 上抓取的文件**。
凡是没有证据支撑的判断，一律写 `未核实`，不写成结论。

> **本文状态**：§1 的"未做"清单与 §3 的表格主体是 P1.1 的记录。
> P1.2 真实取源并编译了本轮依赖之后更新的行已就地标注 **P1.2**；
> 新增的实测结论集中在 §5.4（Boost）与 §6 表格。
> §1 保留了 P1.1 当时的原始声明，它是审计轨迹的一部分，不要当成当前状态读。
>
> **2026-11-02 发行准备补充**：当前产品许可与链接面以
> `data/manifests/product-dependencies.json` 和 `product-package-files.inc` 为准。
> RapidJSON 头实际进入 OpenCC runtime，完整固定上游许可已补入产品包；TCLAP 仅为未打包的
> OpenCC 命令行工具依赖。utfcpp 在固定检出中的许可正文是 BSL-1.0，下面历史表的 MIT 标注错误。
> X11 的 keysym.h 与 keysymdef.h 声明均随包保留。当前包规则校验实际 Windows 检出文件的精确字节，
> 不把 CRLF 工作树哈希混称为原始 Git blob 哈希。以上技术核实不替代主项目许可或词表再分发批准。

## 1. 方法与边界

已做：

- 只读 GET 取官方 `api.github.com` / `raw.githubusercontent.com` / `github.com` 的发布、tag、
  提交、文件清单与许可证文本；按父仓库记录的子模块 SHA 取文件（不使用镜像、不使用预编译包）。
- 读检出树内的构建脚本与 CMake 逻辑（**未执行**任何上游脚本）。
- 检出树内文件指纹：`sha256sum LICENSE README-windows.md build.bat CMakeLists.txt env.bat.template`。

未做（因此不声称）：

- **未编译任何依赖**；未运行 `build.bat` / `install-boost.bat` / `install-plugins.sh`。
- **未获取任何子模块工作副本**（6 个都停在未初始化状态）。
- **未做签名或作者身份验证**（tag 无 PGP 签名）。
- 未逐行审查任何依赖的完整源码树；许可证判断只覆盖**许可证文件本身**。

## 2. 状态图例

| 记号 | 含义 |
| --- | --- |
| 已核实 | 在该固定提交上取到文件并读过正文 |
| 部分核实 | 取到了文本，但用途/链接面未实测 |
| 未核实 | 没有取到或没有检查；不作为合规依据 |

## 3. 汇总表

| # | 名称 | 用途 | 分级 | 版本约束（证据） | 已固定？ | 许可证 | 额外分发义务 | 未经核实的嵌套依赖 | 进入下一步最小构建？ |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | librime | 引擎底座 | 核心 | tag 1.17.0 | ✅ 完整 SHA | BSD-3-Clause（已核实） | 保留版权声明与免责；不得暗示背书（第 3 条） | 见 #7–#12 | ✅ |
| 2 | yaml-cpp | 配置/schema 解析 | 核心必需 | `find_package(YamlCpp REQUIRED)` `CMakeLists.txt:126` | ✅ `2f86d137…` | MIT（已核实） | 随分发附 MIT 文本 | 无（该 SHA 无 `.gitmodules`） | ✅ |
| 3 | leveldb | 用户词库存储 | 核心必需 | `REQUIRED` `CMakeLists.txt:134` | ✅ `99b3c03b…` | BSD-3-Clause（已核实） | 同上 | 有：`third_party/googletest`、`third_party/benchmark`（本方案不构建） | ✅ |
| 4 | marisa-trie | Trie 词库格式 | 核心必需 | `REQUIRED` `CMakeLists.txt:139` | ✅ `3e87d53b…` | **双许可 BSD-2 OR LGPL-2.1+**（已核实正文） | 取 BSD-2 分支；若改取 LGPL 需重估链接与附带义务 | 无 | ✅ |
| 5 | OpenCC | 繁简转换 | 核心必需（合并构建下） | `REQUIRED` `CMakeLists.txt:144` | ✅ `556ed224…` | Apache-2.0（已核实） | 须附 Apache-2.0 文本、保留声明、标注修改；**该提交无 `NOTICE`**（探测 404） | **有，7 项，见 #13–#19** | ✅ |
| 6 | google-glog | 日志后端 | **可选** | `find_package(Glog REQUIRED)` 仅在 `if(ENABLE_LOGGING)` 内 `CMakeLists.txt:76-83` | ✅ `7b134a5c…` | BSD-3-Clause（已核实） | 同上 | 无（gflags 走 find_package，且 `build.bat:119` 传 `WITH_GFLAGS=OFF`） | ⚠️ 取决于是否开日志，见 §5.2 |
| 7 | gflags | glog 可选后端 | 可选→本方案不用 | `find_package(Gflags)` 无 REQUIRED `CMakeLists.txt:78` | ❌ 未固定 | 未核实 | — | — | ❌ |
| 8 | googletest | 单元测试 | **仅测试** | `find_package(GTest REQUIRED)` 在 `if(BUILD_TEST)` `CMakeLists.txt:118-119` | ✅ `f8d7d77c…` | BSD-3-Clause（已核实） | 同上 | 无 | ❌（`build.bat:33` 默认 `build_test=OFF`） |
| 9 | Boost | regex / interprocess / signals2 / uuid / algorithm / crc / unordered | **核心必需** | `find_package(Boost 1.74.0 REQUIRED COMPONENTS regex)` `:66`；`find_package(Boost 1.77.0)`（无组件）`:68`；`README-windows.md:9` 写 `>=1.83`；`env.bat.template:6` 默认 `boost-1.89.0` | ✅ **P1.2 已固定**：官方 `boost_1_89_0.7z`，SHA-256 逐字比对通过 | **已核实**：`LICENSE_1_0.txt`（Boost Software License 1.0），sha256 `beb8e42e9d6b4284e03304d05a81a0755200a965fc8d0a5e0aea1e84cf805d6e` | 随分发附 BSLE-1.0 文本并保留声明 | ✅ **纯头文件即可编译链接**，见 §5.4 |
| 10 | darts-clone（树内 vendored） | OpenCC 的 ocd 字典格式 | 随 OpenCC | `include/COPYING.darts-clone` 存在；`ENABLE_DARTS` 默认 ON（OpenCC `CMakeLists.txt:30`） | 随 #5 固定 | BSD-3-Clause（已核实正文，来自 librime 自带的 `include/COPYING.darts-clone`） | 同上 | 无 | ✅（头文件形式，无需单独构建） |
| 11 | utfcpp（树内 vendored） | UTF-8 处理 | 核心（被 rime 源码使用） | `include/utf8.h:1` `Copyright 2006 Nemanja Trifunovic` | 随 #1 固定 | MIT（已核实正文） | 同上 | 无 | ✅ |
| 12 | X11 keysym 头（树内 vendored） | 键名映射 | 核心（头文件） | `CMakeLists.txt:152` `find_path(X11Keysym X11/keysym.h)`；`include/X11/keysym.h:1-3` `Copyright 1987, 1998 The Open Group` | 随 #1 固定 | X11/MIT 式许可（已核实正文） | 保留该声明 | 无（只要头文件，不链 X11 库） | ✅ |
| 13 | marisa-0.2.6（OpenCC 内置） | OpenCC 自身依赖 | 随 OpenCC 构建 | OpenCC `CMakeLists.txt:209-211` `add_subdirectory(deps/marisa-0.2.6)`（`USE_SYSTEM_MARISA=OFF` 默认） | 随 #5 固定（但**不是独立 SHA**） | 该副本内有 `deps/marisa-0.2.6/COPYING.md` | 同 #4 | 无 | ⚠️ **同一库出现两个版本**（0.2.6 与 0.3.1） |
| 14 | rapidjson-1.1.0（OpenCC 内置） | JSON | 随 OpenCC | 该副本内 35 个头文件，目录树探测**无 LICENSE 文件** | 随 #5 固定 | **未核实** | 未知 | — | 未确定（P1.2 实测是否参与链接） |
| 15 | tclap-1.2.5（OpenCC 内置） | 命令行解析 | 疑似仅工具 | 该副本 29 个文件，**无 LICENSE 文件** | 随 #5 固定 | **未核实** | 未知 | — | ❌ 推测不进库构建，**未核实** |
| 16 | googletest-1.15.0（OpenCC 内置） | OpenCC 测试 | 仅测试 | `ENABLE_GTEST` 默认 OFF（`CMakeLists.txt:28`） | 随 #5 固定 | 有 `LICENSE` 文件，正文**未读** | — | — | ❌ |
| 17 | pybind11-2.13.1（OpenCC 内置） | Python 绑定 | 仅数据/绑定构建 | `BUILD_PYTHON` 默认 OFF（`CMakeLists.txt:31`） | 随 #5 固定 | 有 `LICENSE` 文件，正文**未读** | — | — | ❌ |
| 18 | google-benchmark（OpenCC 内置） | 基准 | 仅基准 | `ENABLE_BENCHMARK` 默认 OFF（`CMakeLists.txt:29`） | 随 #5 固定 | 有 `LICENSE` 文件，正文**未读** | — | — | ❌ |
| 19 | darts-clone-0.32（OpenCC 内置） | 与 #10 同源 | 随 OpenCC | `USE_SYSTEM_DARTS` 默认 OFF（`CMakeLists.txt:32`） | 随 #5 固定 | 该副本内**无 LICENSE**；同一项目许可已由 #10 核实 | 同上 | — | ✅（头文件） |
| 20 | librime `data/`（约 5.2 MB） | 最小词库 + 测试配置 | **仅数据构建**（`BUILD_DATA` 默认 OFF `CMakeLists.txt:21`） | 检出树内 `data/minimal/*`、`data/test/*` | 随 #1 固定 | **未核实**（词库授权与代码授权分离，不得由 BSD-3 推定） | 未确定 | — | ❌ 本阶段不使用 |

## 4. librime 自身

- 许可证文件：`LICENSE`，sha256 `44c39955377bb15d825f8142de500ed832309178ce34624c5d1a1f00761bb1ee`。
  正文为 BSD-3-Clause 三条款（含非背书条款），版权行为 `Copyright (c) 2014, RIME Developers`。
- 每个源文件头部重复声明 `Distributed under the BSD License`（例：`src/rime/engine.h:2-3`、
  `src/rime/composition.h:2-3`、`src/rime/setup.cc:2-5`）——**这些声明必须随源码保留**。
- 检出树内**没有任何预编译二进制**：`find` 统计 `*.dll/*.lib/*.a/*.exe` 结果为 0，
  因此不存在"来源不明的 DLL"风险。
- 上游 `CHANGELOG.md:1-2` 标注 `## 1.17.0 (2026-06-05)`，与 tag 注解日期一致。

## 5. 两个需要特别记录的审查点

### 5.1 上游 Windows 取源脚本会写入源码树

`build.bat:7` 会 `copy env.bat.template env.bat`，`build.bat:79-80` 的默认安装前缀是
`%RIME_ROOT%`（即上游检出目录本身），`CMakeLists.txt:167` 又 `link_directories(${PROJECT_SOURCE_DIR}/lib)`。
后果：按上游文档流程走会把依赖产物写进 `third_party/librime/`，**破坏"上游字节干净"这条规则**。
EpoCanva 侧的方案因此不改上游，只把前缀改到项目内被忽略的目录（见
`docs/build/librime-windows-baseline.md` 第 3 节）。

### 5.2 关闭日志对编译与链接的实际影响

- `ENABLE_LOGGING=OFF` 时：`find_package(Glog REQUIRED)`（`CMakeLists.txt:76-83`）整块不执行；
  `src/CMakeLists.txt:48` 的 `${Glog_LIBRARY}` 展开为空；
  源码侧 `src/rime/setup.cc:10-14` 改为包含 `src/rime/no_logging.h`（该文件确实存在，2457 字节），
  `src/rime/common.h:28-32` 的 `<glog/logging.h>` 也在同一开关之下。
  → **glog 与 gflags 可以完全从构建中消失**，这是源码层面的证据，不是猜测。
- 但 `dbghelp` 只在 `if(Glog_FOUND AND WIN32)`（`src/CMakeLists.txt:61-63`）时加入，
  关掉日志后该库也不再需要。
- **P1.2 已实测**：`ENABLE_LOGGING=OFF` 下没有任何残留符号依赖导致编译或链接失败 ——
  未取 glog/googletest 子模块的前提下，四个依赖 + librime 全部构建成功。
  全部 19 个步骤日志里 `glog`、`dbghelp` 命中数为 **0**；`gtest`/`GTest` 的**唯一**一次出现是
  我们传给 OpenCC 的参数行 `-DENABLE_GTEST:BOOL=OFF`（`12-opencc.configure.log:23`），
  没有任何 googletest 被配置、获取或编译；librime 自己的配置日志里 `glog`/`gtest` 命中 0。
  `ICU`/`iconv` 在 librime 配置与构建日志里同样命中 0。
  产出的 `rime.dll` 导入表只有 `KERNEL32.dll` 与 `USER32.dll`。
  `SetupLogging()` 的函数体被 `#ifdef RIME_ENABLE_LOGGING` 整块去掉（`src/rime/setup.cc:71-95`），
  所以即使 traits 里给了 `log_dir` 也不会写任何日志文件
  （生命周期程序实测：日志目录在整轮生命周期后仍为空）。

### 5.3 MinGW 分支会引入网络相关库

`src/CMakeLists.txt:66-69` 在 `if(MINGW)` 时追加 `wsock32 ws2_32 bcrypt`（注释说明来自 boost uuid 的修复）。
本项目使用的是 MSVC 路径，不会命中这一分支；但这条记录必须留着，因为
**一旦改用 MinGW 构建，静态导入表里就会出现 `ws2_32`**，
届时"静态导入表里没有网络库"这条检查会立刻失效——这正是 `docs/security/threat-model.md` 3.8
把导入表检查限定为"仅静态导入"的原因之一。

### 5.4 Boost 需要哪些编译产物（P1.2 实测，推翻 P1.1 的推论）

P1.1 把"上游 Windows 分支 `find_package(Boost 1.77.0)` 不要求 `COMPONENTS regex`、
树内又没有 `boost_regex.lib`"列为**链接阻塞**。P1.2 按固定版本真实源码与真实编译链接
证明该推论不成立：

1. librime 用到的 8 个 Boost 库里，**7 个在 1.89.0 没有编译源**：
   实测 `libs/<name>/src/*.cpp` 计数为 0 —— `unordered`、`signals2`、`uuid`、
   `interprocess`、`algorithm`、`crc`、`range`。
2. `regex` 是唯一有编译源的（`libs/regex/src` 恰 2 个文件：
   `posix_api.cpp`、`wide_posix_api.cpp`），它们实现的只是 **C POSIX 兼容层**
   （`regcomp`/`regexec`/`regwcomp`）。`libs/regex/build/Jamfile.v2:152-157` 的
   `lib boost_regex : ../src/$(SOURCES)` 只编这两个文件，且对使用者导出
   `<define>BOOST_REGEX_NO_LIB=1`。
   `libs/regex/CMakeLists.txt:11` 更进一步：`add_library(boost_regex INTERFACE)`，
   即 Boost.Regex 的 CMake 目标本身就是**不产出任何 .lib 的接口库**。
3. librime 只用 `boost::regex` 的 C++ 模板接口
   （`src/rime/algo/calculus.h:12`、`algo/encoder.h:11`、`gear/recognizer.h:10`、
   `gear/speller.h:10`、`gear/translator_commons.h:10`），
   从不包含 `boost/cregex.hpp`。触发 auto-link 的 `#pragma comment(lib, ...)` 只写在
   `boost/regex/v5/cregex.hpp:29-30`，而 `boost/regex.hpp` → `regex/config.hpp` +
   `regex/v5/regex.hpp`，后者第 28-71 行的包含列表里**没有** `cregex.hpp`。
   因此编译 `boost/regex.hpp` 不会要求任何库文件。
4. 上游自身的 Windows 流程与此一致：`install-boost.bat:19-20` 只执行
   `bootstrap.bat` 然后 `.\b2 headers`，**不编译任何 Boost 库**；
   `.github/workflows/windows-build.yml:68-69` 在生产 `rime.dll` 的流水线上用的就是这一条。
5. 本轮实测：用与 `third_party/librime/CMakeLists.txt:68` 相同的
   `find_package(Boost 1.77.0)`（无组件、`Boost_USE_STATIC_LIBS/STATIC_RUNTIME=ON`）
   指向解压后的 `boost_1_89_0` 树，得到
   `Boost_FOUND = TRUE`、`Boost_LIBRARIES` **为空**，
   随后把上述 8 个库的用法编进一个 MSVC x64 /MT 程序并成功链接、运行；
   产物 `dumpbin /dependents` 只有 `KERNEL32.dll`、`USER32.dll`、`ADVAPI32.dll`。

结论：**没有 `boost_regex.lib` 不是阻塞，而是预期形态。**
`src/CMakeLists.txt:47` 的 `${Boost_LIBRARIES}` 展开为空是正确结果。
因此 `docs/build/librime-windows-baseline.md` §10 原先准备的
"给 `CMakeLists.txt:68` 补 `REQUIRED COMPONENTS regex`" 补丁建议**不再需要**，
且若真的加上反而会要求一个不存在的库。该建议已在原文标注作废。

## 6. 未核实清单（明确列出，不掩盖）

| 项 | 状态 | 需要谁做什么 |
| --- | --- | --- |
| Boost 的许可证文本与版本 | **已核实（P1.2）**：官方 `archives.boost.io/release/1.89.0/source/boost_1_89_0.7z`，下载后 sha256 逐字比对为 `d8b0c1d593d68fe359616aa2fae4b3e258ca1f90ed57eb46c5494c29cdd1d49d`（与官方 `boost_1_89_0.7z.json` 完全一致，元数据 commit `ef7fea34711a189472893b88205b1dd3c275677b`）；`LICENSE_1_0.txt` = Boost Software License 1.0，sha256 `beb8e42e9d6b4284e03304d05a81a0755200a965fc8d0a5e0aea1e84cf805d6e` | 已闭环，见 §5.4 |
| Boost.Regex / Boost.Interprocess 需要哪些编译库 | **已核实（P1.2）**：librime 用到的 Boost 组件在本提交全部按头文件方式使用，`Boost_LIBRARIES` 为空是正确结果 | 已闭环，见 §5.4 |
| rapidjson-1.1.0 的许可正文 | **未核实**（OpenCC 副本内无 LICENSE 文件）。P1.2 实测它**进入 `libopencc` 本体**：`deps/opencc/src/CMakeLists.txt:9-11` 把该目录加进包含路径，`src/Config.cpp` 实际使用其头（rapidjson 是头文件库，不产出独立二进制） | 从官方仓库按 1.1.0 取 MIT 文本并登记 |
| tclap-1.2.5 的许可正文与是否进库 | **部分核实（P1.2）**：**不进库，只进命令行工具**——使用点是 `src/CmdLineOutput.hpp`、`src/tools/CommandLine.cpp`、`src/tools/DictConverter.cpp`、`src/tools/PhraseExtract.cpp`，而 `src/CMakeLists.txt:48-68` 的 `LIBOPENCC_SOURCES` 列表里没有它们。本轮按上游默认目标构建时这三个工具确实被编译并安装到 `prefix/bin`。许可正文仍未取到 | 从官方源取 MIT 文本；若最终不随包分发工具，可据此把它移出分发面 |
| OpenCC 的 `data/CMakeLists.txt:4-5` 调用 Python 生成词典，是否属于默认目标 | **已核实（P1.2）：是。** `data/CMakeLists.txt:39-44` 写的是 `add_custom_target(Dictionaries ALL DEPENDS ...)`，`:150-155` 又 `install(FILES ${DICT}.ocd2 ...)`；`:1` 的 `find_package(PythonInterp REQUIRED)` 在配置阶段无条件执行。因此 OpenCC 的默认构建与 `--target install` 都要 Python，跳过词典会让 `cmake --install` 在缺失的 `.ocd2` 上失败 | 已闭环。本轮用本机既有 `C:\Python314\python.exe`（3.14.3）完成，未安装任何软件 |
| 同一进程内 marisa 0.2.6（OpenCC 内置）与 marisa 0.3.1（librime 直接依赖）共存后果 | **已核实（P1.2），并且已规避**：OpenCC 的 `USE_SYSTEM_MARISA=ON` 分支确实可用，但缺两件事——它不给外部 marisa 加包含目录（`src/CMakeLists.txt:6-8` 只在内置分支加）、并把 `marisa` 当裸名字链接（`:118`）；且 marisa 0.3.1 的公共头用了 `std::string_view` 与嵌套命名空间，而 OpenCC 对 MSVC 不设置 C++ 标准。本轮以 `CMAKE_CXX_STANDARD=17` 加上项目自有 `CMAKE_PROJECT_opencc_INCLUDE` 钩子补齐，未改上游，OpenCC 直接链固定的 marisa 0.3.1，树内 `deps/marisa-0.2.6` 未被构建 | 已闭环。钩子文件与理由见 `docs/build/librime-windows-baseline.md` |
| librime `data/` 内词库文本的授权 | **仍未核实，且已发现明确的不兼容信号**：`data/minimal/cangjie5.dict.yaml` 头部直接写着 `# License: GPL`，并注明单字码表来自 `www.chinesecj.com`；`data/minimal/luna_pinyin.dict.yaml` 头部列出 CC-CEDICT、Android PinyinIME、Chewing、三拼簡繁詞庫、opencc 等多个来源，逐项授权未在文件内声明；`essay.txt` 由外部 `rime-essay` 仓库提供（`README.md:119`）。**注意**：`third_party/librime/tools/CMakeLists.txt:56-68` 会在 `BUILD_SHARED_LIBS=ON` 时把这些文件复制进构建树，所以即使 `BUILD_DATA=OFF`，词库文本仍会出现在 `build/` 内 | 发布任何词表前逐文件审查；GPL 项必须按 `AGENTS.md` 走 ADR，不由实现者自行判断 |
| 上游 tag 的作者身份/签名 | **确认无签名**（不是"未检查"，是检查结果为无） | 若要更强保证，需要上游提供签名或改用可验证渠道，不由本项目擅自假设 |
| `CMAKE_MSVC_RUNTIME_LIBRARY` 与 `cmake/cxx_flag_overrides.cmake` 两套 CRT 机制同时存在时哪套生效 | **已核实（P1.2）：按各工程自己的 `cmake_minimum_required` 分流。** ≥3.15 的工程（marisa 3.22、yaml-cpp 显式 `cmake_policy(SET CMP0091 NEW)`）走 `CMAKE_MSVC_RUNTIME_LIBRARY` 抽象，并且 CMake 会从 flags 里去掉手写运行时开关，结果是**恰好一个 `/MT`**；<3.15 的工程（leveldb 3.9、OpenCC 3.5、librime 3.12）该变量无效，`/MT` 完全来自 `*_FLAGS_INIT` 覆盖文件 | 已闭环。证据：`16-rime.build.log` 真实 cl 命令行 `/MT=7 /MD=0`；9 个产物的 `dumpbin /directives` 全部是 `RuntimeLibrary=MT_StaticRelease`，`MD_DynamicRelease`/Debug CRT 均为 0 |
| OpenCC 以 `/MD` 编译时链接 `/MT` 的 `marisa.lib` 会怎样 | **已核实（P1.2）：链接期硬失败**，不是静默混用 —— `libcpmt.lib(locale0.obj) : error LNK2038: 检测到"RuntimeLibrary"的不匹配项: 值"MT_StaticRelease"不匹配值"MD_DynamicRelease"` | 这条是"统一 CRT"要求可被机器检查的证明；本轮故意制造过一次以确认检查不是纸面规则 |

## 7. 结论

1. librime 本体与其 6 个直接子模块的许可证均已**在固定提交上核实**，全部为宽松许可
   （BSD-3 / BSD-2 / MIT / Apache-2.0），marisa 的双许可已显式选择 BSD-2 分支。
2. ~~**唯一"未固定且未核实"的必需依赖是 Boost**，它同时是当前最小构建的最大阻塞点。~~
   **P1.2 更新**：Boost 已固定（官方 `boost_1_89_0.7z`，SHA-256 逐字比对通过）、
   许可证已核实（BSLE-1.0），并且**不是阻塞**——它需要的编译产物数量是 0，见 §5.4。
   现在仍未核实的是 rapidjson-1.1.0 与 tclap-1.2.5 的许可证正文，
   以及 `data/minimal` 词库（其中 `cangjie5.dict.yaml` 自述 `License: GPL`）。
3. 本阶段不引入任何外部插件（`legacy/lua/predict/octagram/proto`），
   证据：`plugins/` 下只有 `CMakeLists.txt`、`plugin.cc`、`plugins_module.cc` 三个普通文件，
   插件列表来自环境变量 `RIME_PLUGINS`（`plugins/CMakeLists.txt:24-25`），不设即不构建。
4. 合规上必须新增的负担：一份随分发的第三方许可证汇编（包含 BSD-3/BSD-2/MIT/Apache-2.0 正文），
   以及**词库授权的独立调查**。EpoCanva 自身许可证仍未定，本文不推导它的授权条款。
5. 不存在"仅凭上游 README 授权表即认定合规"的情况：本文每一项都给了证据路径或明确标注未核实。
