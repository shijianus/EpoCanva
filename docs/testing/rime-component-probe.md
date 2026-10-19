# 编译进引擎的自有组件可达性验证（P1.4）

本文描述 `tools/rime_component_probe`、`engine/rime_components/probe/` 与夹具
`tests/fixtures/rime_probe/` 的**构建方法、运行方法、判定口径、失败注入方法**。
阶段结论见 `docs/reports/P1.4-component-probe.md`；接入形态的决策见
`docs/adr/0004-compiled-in-rime-components.md`。

---

## 1. 这套测试证明什么，不证明什么

**证明**（全部由运行时可观察量判定，不由"编译通过"或"源码里有这段"判定）：

1. 用 `-Mode component-probe` 产出的 `rime.dll` 里，确实存在名为 `epocanva_probe` 的
   rime 模块，并且它的 `initialize()` 被 librime 自己的模块装载机制调用过；
2. 该模块注册的 `epocanva_probe_processor` 被真实引擎构造为实例；
3. 按键确实进入这个自有 `Processor::ProcessKeyEvent()`，字母被它放行后
   仍然形成与 P1.3 夹具一致的组合；
4. 夹具指定的那个测试键被**该组件**消费（计数增量 + `process_key` 返回 True），
   且不改变 preedit、不产生提交；
5. 会话销毁后实例被释放，连续多轮创建/销毁后活实例数回到轮初值。

**不证明**（每一项都是"未实现/未验证"，不得写成已通过）：

| 项 | 状态 |
| --- | --- |
| 英文补全、预测、词频学习、形态变化、纠错、自动分段、缩写展开 | **未实现**。`AGENTS.md` §1 的要求完全未触及 |
| 任何语言学输出 | 候选文本与注入按键**逐字节相同**（`echo_translator` 原样回显） |
| Unicode / CJK / 非 ASCII / 字素簇 / 屏幕列换算 | **未验证**；测试数据全为 ASCII |
| 词典、用户词库、学习数据链路 | **不使用** |
| 生产可用的组件、插件系统、脚本扩展 | **不提供**；探针组件的职责只是可达性见证 |
| 平台前端（TSF / Android IME） | **未实现** |

## 2. 前置条件

- 已完成 `-Mode baseline`（P1.2 的产物在位即可）：四个依赖库装在
  `build/librime-baseline/prefix/lib/`，`rime.dll` 在
  `build/librime-baseline/rime/install/lib/rime.dll`，Boost 头树已解压。
  **本轮不重新下载、不重新编译依赖**：`component-probe` 模式复用同一套依赖库，
  并在脚本里重新断言它们的来源。
- 工具链：VS2022 Professional（生成器 `Visual Studio 17 2022`、`-A x64`）、
  其自带 CMake/CTest `3.31.6-msvc6`。本机 CMake 不在常规 PATH，
  须先进入开发者环境（命令见 `docs/build/bootstrap.md`）。
- `cmake_language(DEFER …)` 需要 CMake ≥ 3.19；本机固定 3.31.6。

## 3. 构建自有版引擎

```sh
# 只做引擎 DLL（复用 baseline 的依赖；-Fresh 只删本模式自己拥有的 rime 目录）
tools/build_librime_baseline.ps1 -Mode component-probe -Fresh -Verbose_Build
```

产物：`build/librime-component-probe/rime/install/lib/{rime.dll,rime.lib}`。
**`build/librime-baseline/` 不被写入**（脚本在 component-probe 模式下把
`$Prefix` 指向 baseline 树并只读复用，`-Fresh` 也只删除
`build/librime-component-probe/rime`）。

默认模式 `-Mode baseline`（或不带参数）与 P1.2 完全一致，
且脚本会拒绝任何"默认构建悄悄混入自有组件"的结果。

## 4. 构建并运行测试程序

```sh
cmake -S tools/rime_component_probe \
      -B build/librime-component-probe/probe \
      -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_CONFIGURATION_TYPES:STRING=Release \
      -DRime_Install_Prefix:PATH=<repo>/build/librime-component-probe/rime/install \
      -DBaseline_Rime_Prefix:PATH=<repo>/build/librime-baseline/rime/install \
      -DEpoCanva_Rime_Input_Replay_Exe:FILEPATH=<repo>/build/librime-baseline/replay/Release/rime_input_replay.exe

cmake --build build/librime-component-probe/probe --config Release

ctest --test-dir build/librime-component-probe/probe -C Release \
      --output-on-failure --no-tests=error
```

三个前缀/路径变量都是必填或可选降级：缺 `Rime_Install_Prefix` 或
`Baseline_Rime_Prefix` 在**配置阶段** `FATAL_ERROR`；
不传 `EpoCanva_Rime_Input_Replay_Exe` 则不注册回放回归用例（并打印原因），
不会静默少跑测试。夹具缺失同样在配置阶段失败。

## 5. CTest 用例

| 注册名 | 可执行文件 | `--mode` | 标签 | TIMEOUT | 判定 |
| --- | --- | --- | --- | --- | --- |
| `librime.component_probe.compiled_in_module_and_processor` | `rime_component_probe` | `probe` | `librime;component_probe;matrix` | 180 | 仅退出码 |
| `librime.component_probe.negative_baseline_engine` | `rime_component_probe_baseline` | `expect-module-absent` | `librime;component_probe;negative` | 180 | 仅退出码 |
| `librime.component_probe.negative_schema_without_component` | `rime_component_probe` | `expect-component-absent` | `librime;component_probe;negative` | 180 | 仅退出码 |
| `librime.component_probe.replay_regression_on_component_engine` | 复制的 `rime_input_replay.exe` | P1.3 矩阵 | `librime;component_probe;regression` | 180 | 仅退出码 |
| `librime.component_probe.replay_missing_fixture_on_component_engine` | 同上 | P1.3 负向 | `librime;component_probe;regression;negative` | 180 | 仅退出码 |

**不使用 `PASS_REGULAR_EXPRESSION`，也不使用 `WILL_FAIL`。**
两个负向用例是对"引擎没有这个能力"的**正向断言**，它们自己必须返回 0；
一旦组件其实可达、或引擎真的消费了那个键，它们返回 18。

两个可执行文件来自同一份 `main.cc`，但分别链接 **component 构建**与
**baseline 构建**的 `rime.lib`，并各自在自己的输出目录里放置对应的那一个
`rime.dll`（Windows 优先在 EXE 自身目录解析隐式加载的 DLL，因此不依赖 PATH，
且一个目录里只可能有一个 `rime.dll`）。运行时程序再用
`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)` 反查真正提供了
`get_version` 的模块，与 `--expect-dll` 归一化后逐字比较。

## 6. 事件矩阵与预期（`probe` 模式）

`mask` 的单位是 librime 自己的修饰位（`src/rime/key_table.h:13-43`：bit0 Shift、
bit1 Lock、bit2 Control、bit3 Mod1/Alt、bit30 Release）。keysym 取自上游
`include/X11/keysymdef.h`。`d{…}` 是组件计数的**增量**，来自真实读回。

| # | 键 | keysym | mask | 引擎 handled | 预期 preedit | 预期候选 | 预期 commit | pk | passed | consumed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | a | `0x0061` | 0 | 是 | `a` | 1 | 无 | +1 | +1 | 0 |
| 2 | b | `0x0062` | 0 | 是 | `ab` | 1 | 无 | +1 | +1 | 0 |
| 3 | c | `0x0063` | 0 | 是 | `abc` | 1 | 无 | +1 | +1 | 0 |
| 4 | **F6** | **`0xffc3`** | 0 | 是 | `abc`（不变） | 1（不变） | 无 | +1 | 0 | **+1** |
| 5 | Shift+F6 | `0xffc3` | 1 | **否** | `abc` | 1 | 无 | +1 | +1 | 0 |
| 6 | BackSpace | `0xff08` | 0 | 是 | `ab` | 1 | 无 | +1 | +1 | 0 |
| 7 | Escape | `0xff1b` | 0 | 是 | 无 | 0 | 无 | +1 | +1 | 0 |
| 8–10 | a,b,c | `0x0061..63` | 0 | 是 | `a`/`ab`/`abc` | 1 | 无 | +1 | +1 | 0 |
| 11 | Return | `0xff0d` | 0 | 是 | 无 | 0 | **`abc`** | +1 | +1 | 0 |

矩阵之后再读一次状态：**不得**再读到 commit（`get_commit` 会取走队列）。

隔离块：新会话 B 上 `x`→`xy`（passed +1 各一次），F6 on B（consumed +1、
B 的 `xy` 不变）；同一进程内 A 上再按 F6（consumed +1，A 无 preedit、无 commit），
回读 B 仍为 `xy`。生命周期块：3 轮"建会话 → 按 `q` 必须组合 → 销毁"，
每轮断言活实例数回到轮初、构造增量与析构增量相等。

汇总不变量（`counters.final`）：`live == 0`、`constructed == destroyed`、
`pk_calls == passed + consumed`、`consumed == 3`、`passed == 15`、`pk_calls == 18`。
**没有任何"构造了几次"的硬编码假设**：每轮只断言增量关系。

## 7. 输出字段口径

```
[probe] ev=04 key=0xffc3 'F6' mask=0      handled=1 d{pk=1 pass=0 consumed=1} schema=epocanva_test_probe composing=1 disabled=0 preedit="abc"[len=3] cands=1 cand0="abc" input="abc" commit=-
```

| 字段 | 来源 | 单位 / 哨兵 |
| --- | --- | --- |
| `handled` | `process_key()` 返回值 | 1=引擎消费；0=未消费 |
| `d{pk pass consumed}` | 组件诊断计数增量（`find_module()->get_api()->get_counters()`） | 事件计数 |
| `schema` | `get_status().schema_id` | UTF-8 码元；`select_schema()` 的返回值**不作为判据** |
| `preedit` | `get_context().composition.preedit` | UTF-8 码元；**`-` 表示指针 NULL** |
| `len` | `composition.length` | **对 preedit 的字节偏移** |
| `cands/cand0` | `menu.num_candidates` / `menu.candidates[0].text` | 个数 / 字节串 |
| `input` | `get_input()` | 无 free 接口，立即复制且不释放 |
| `commit` | `get_commit().text` | **`-` 表示返回 False** |
| `constructed.by.A`、`lifetime.*`、`counters.*` | 同一诊断接口 | 事件计数 |

诊断接口的所有权规则（`probe_diagnostics.h` 头部逐条写明）：
计数写入**调用方自己分配的定长结构**；`get_api()` 与 `build_id()` 返回
`rime.dll` 内的**静态存储**指针，调用方**不释放**；
`finalize()` 之后**不再使用该接口**（`finalize()` 会 `UnloadModules()` +
`Registry::Clear()`，`src/rime_api_impl.h:58-63`）。
`reset_counters_when_idle()` 在有活实例时**必须返回 False 且不改任何计数**，
这一条本身也是被断言的（`matrix` 模式里显式检查）。

## 8. 失败注入（证明这套测试能失败）

结构级（无需重编；本轮把可执行文件单独拿出来逐条跑过，退出码为实测）：

| 注入 | 预期 | 实测 |
| --- | --- | --- |
| `--expect-dll` 指向别的文件 | 4 | **4**，打印 expected/actual 两条路径 |
| `--run-root` 不在守护目录名 `rime-component-probe-run` 下 | 6 | **6**，并声明拒绝删除 |
| `--fixture-dir` 指向无夹具 yaml 的目录 | 5 | **5**，逐行打印 `fixture.absent` |
| 缺必需参数 | 2 | **2**，打印 usage |
| 未知 `--mode` 值 | 2 | **2**，`unknown --mode value 'bogus'` |
| 夹具**副本**里让 `- epocanva_probe_processor` 出现 2 次 | 5 | **5**，`removed 2`；副本在 `build/` 下，正式夹具未被改动 |

最后一行证明 `expect-component-absent` 的"派生副本"不是摆设：
如果那一行因为夹具被改动而匹配到 0 次或多次，测试直接失败，
而不是"什么都没删掉、于是等价于正向用例"还跑绿。

**交叉注入（本轮实际执行，不改一个源码字节）** —— 把可执行文件与 DLL 交换放置：

| 组合 | 预期 | 实测 |
| --- | --- | --- |
| `rime_component_probe.exe` + **baseline** `rime.dll` + `--mode probe` | **必须失败** | 退出 **21**，`module.before_setup absent`、`diagnostics=unavailable` |
| `rime_component_probe_baseline.exe` + **component** `rime.dll` + `--mode expect-module-absent` | **必须失败** | 退出 **18**，`ASSERT FAIL … got "FOUND"` / `got "AVAILABLE"` |

这两条是关键证据：结果由**实际加载的引擎二进制**决定，不是由 `--mode` 字符串决定。

负向用例本身（`expect-module-absent` / `expect-component-absent`）也是可失败断言：
两者各自返回 0 才算通过，一旦被检引擎其实具备该能力，返回 18；
上表最后一行与交叉注入共同构成"这两条能失败"的证据。

## 9. 隔离与可重建性

- 运行根由 CTest 传入：
  `build/librime-component-probe/probe/rime-component-probe-run/{probe,negative-baseline,negative-schema}`，
  每次运行先删除再重建 `shared/ user/ log/`，父目录名必须恰好是守护名。
- `shared_data_dir`/`user_data_dir`/`log_dir` 全部指向私有目录；
  **不使用 `%APPDATA%\Rime`、不读个人 Rime 配置、不全局安装配置**。
- 负向用例需要的"去掉探针组件的 schema"是**程序自己的运行目录里的派生副本**，
  `tests/fixtures/` 只读；不存在"临时覆盖正式源码后忘记恢复"的可能。
- 已知副作用（都在私有运行目录内）：`select_schema()` 会经
  `Switcher::SetActiveSchema`（`src/rime/switcher.cc:165-172`）写
  `<user>/user.yaml` 的 `var/previously_selected_schema` 与
  `var/schema_access_time/*`。
- 引擎侧隔离：`build/librime-baseline/`（原版）与
  `build/librime-component-probe/`（自有版）互不覆盖；两个引擎在**不同进程**里运行；
  两者 DLL 路径与 SHA-256 都记录在阶段报告里。

### DLL 的 SHA-256 是"这一次构建的身份"，不是可复现性校验和

`link.exe` 会把链接时刻写进 PE 头，产物里还带着 PDB 的 GUID，因此**同一份源码重链一次，
`rime.dll` 的 SHA-256 就会变**。已实测（2026-10-18 的复现轮）：对同一份源码做一次
`-Mode component-probe -Fresh` 全量重编，新 DLL 与上一轮 DLL 相差
**28 个字节**，分布在 5 个区段——PE `TimeDateStamp`（偏移 281–283）、
调试目录里的两处时间戳（2159365–2159367、2159393–2159395、2159421–2159423）与
16 字节 PDB GUID（2261469–2261484）；`rime.lib` 逐字节不变，导出表 571/571 不变。

所以报告里记录哈希的用途只有两个：
**把"跑出某个结果的那个二进制"钉死**，以及**证明 baseline 与 component 是两个不同文件**。
它不能用来宣称"字节级可复现"。要判"组件到底进没进这个 DLL"，
用的是报告 §5.3 那组与时刻无关的量：objects 计数 113→115、
`epocanva_probe` 字符串命中数、导出表差集、以及运行时的诊断计数。
`ENABLE_TIMESTAMP`（`third_party/librime/CMakeLists.txt:31`）管的是
**编译后的 schema 产物**里嵌时间戳，与 `rime.dll` 的这几个字节无关，不是这里的原因。

## 10. 阶段码

| 码 | 含义 | 码 | 含义 |
| --- | --- | --- | --- |
| 0 | 通过 | 11 | `get_status()` 失败 |
| 2 | 参数错误 | 12 | `get_context()` 失败 |
| 3 | `rime_get_api()` 返回 NULL | 13 | `process_key()`/`get_commit()` 返回既非 0 也非 1 |
| 4 | 实际加载模块 ≠ 期望 DLL | 14 | 至少一条值断言失败（probe 模式） |
| 5 | 夹具缺失 / 派生副本删除行数不是 1 | 15 | destroy 后 id 仍可解析或返回 False |
| 6 | 运行目录准备失败 / 守护名不符 | 16 | `get_*`/`free_*` 未配对 |
| 7 | traits 读回不符 | 17 | 日志目录出现文件 |
| 8 | 部署返回值或产物不符 | 18 | 负向预期未成立（引擎比声称的更能干） |
| 9 | `create_session()` 失败 | 19/20 | finalize 后仍有 id / 仍能建 session |
| 10 | schema 选择或读回不符 | 21 | **探针接口不可用**（模块或 get_api 缺失） |
| — | — | 22 | **`get_counters()` 返回 False**（结构版本不符或指针为空） |

任何 API 调用失败都直接返回对应阶段码，**不会用空值继续把测试跑绿**。
`SelectAndConfirmSchema`/`DeployFixture` 只根据**自己**新增的失败数决定返回码，
避免早前某条失败被误报成"部署失败"。
