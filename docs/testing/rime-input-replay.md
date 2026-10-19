# Rime 输入链路无界面回放验证（P1.3）

本文描述 `tools/rime_input_replay` 与夹具 `tests/fixtures/rime_raw/` 的**运行方法、
覆盖范围、判定口径与失败注入方法**。阶段结论见 `docs/reports/P1.3-input-replay.md`。

---

## 1. 这套测试证明什么，不证明什么

**证明**：在固定提交（librime `1.17.0` / `33e78140250125871856cdc5b42ddc6a5fcd3cd4`）
产出的 `rime.dll` 上，按键确实经过
`process_key → processor → segmentor → translator → Context → get_context/get_status`，
并且组合确实经过 `commit → session 提交队列 → get_commit`。
组合、候选、提交文本**全部读自 API 返回值**，程序自己不构造任何一个待断言字符串。

**不证明**（任何一项都是"未实现/未验证"，不得写成已通过）：

| 项 | 状态 |
| --- | --- |
| 英文预测、补全、词频学习、形态变化、纠错、缩写展开 | **未实现**，`AGENTS.md` §1 的要求完全未触及 |
| Unicode / CJK / 非 ASCII 输入 | **未验证**。夹具只承认 7 个 ASCII 键，全部测试数据为 ASCII |
| 词典、语言模型、用户学习数据链路 | **未使用**（夹具的 `translator/dictionary` 缺失，`SchemaUpdate::Run` 因此在 `deployment_tasks.cc:349-353` 直接返回成功） |
| 完整键集行为（方向键、翻页、组合内 Ctrl/Alt 组合） | **未覆盖** |
| 陈旧源码的重新部署检测（`ConfigNeedsUpdate` 走"源文件已改"分支） | **未覆盖**，因为每次运行都重建运行目录 |
| 平台前端（Windows TSF / Android IME） | **未实现** |

## 2. 前置条件

本轮**不重建**依赖。以下三项必须已存在且属于 P1.2 基线（哈希见 P1.2 报告 §6.4 与
`third_party/sources.lock.json`）：

```
build/librime-baseline/rime/install/include/rime_api.h     sha256 85caf744…（与上游 src/rime_api.h 逐字节相同）
build/librime-baseline/rime/install/lib/rime.lib           sha256 024b64d2…
build/librime-baseline/rime/install/lib/rime.dll           sha256 333b5fa0…
```

工具链：VS2022 Professional（生成器 `Visual Studio 17 2022`、`-A x64`）、
其自带 CMake/CTest `3.31.6-msvc6`。本机 CMake 不在常规 PATH 中，须先进入开发者环境：

```powershell
Import-Module "<VS2022 Professional>\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "<VS2022 Professional>" -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64'
```

## 3. 命令

```sh
# 配置（夹具路径默认取 ../../tests/fixtures/rime_raw，无需显式传入）
cmake -S tools/rime_input_replay \
      -B build/librime-baseline/replay \
      -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_CONFIGURATION_TYPES:STRING=Release \
      -DRime_Install_Prefix:PATH=<repo>/build/librime-baseline/rime/install

cmake --build build/librime-baseline/replay --config Release

ctest --test-dir build/librime-baseline/replay -C Release \
      --output-on-failure --no-tests=error
```

回归对照（同一轮必须一起跑）：

```sh
ctest --test-dir build/librime-baseline/smoke -C Release --output-on-failure --no-tests=error
ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error
```

夹具缺失或路径不对时**在配置阶段**就 `FATAL_ERROR`（`tools/rime_input_replay/CMakeLists.txt`
的 `EpoCanva_Rime_Raw_Fixture_Dir` 检查），不会拖到运行期变成谜团。

## 4. CTest 用例

| 注册名 | 标签 | TIMEOUT | 判定 |
| --- | --- | --- | --- |
| `librime.input_replay.raw_schema_key_event_matrix` | `librime;input_replay;matrix` | 120 | 仅退出码 |
| `librime.input_replay.raw_schema_missing_fixture_fails_closed` | `librime;input_replay;negative` | 120 | 仅退出码 |

**不使用 `PASS_REGULAR_EXPRESSION`，也不使用 `WILL_FAIL`**：两者都可能把非零退出变成绿色。
负向用例不是"把失败反过来"，而是对"引擎没有能力"的**正向断言**，所以它自己返回 0
才算通过；一旦夹具其实可达、或引擎真的组合出了东西，它返回 18。

## 5. 事件矩阵与预期

`mask` 恒为 0（librime 修饰键位掩码：bit0 Shift、bit2 Control、bit30 Release，
`src/rime/key_table.h:15-43`）。keysym 取自上游 `include/X11/keysymdef.h`。

| # | 键 | keysym | 应被消费 | 预期 preedit | 预期候选数 | 预期 commit |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | BackSpace | `0xff08` | 否 | 无 | 0 | 无 |
| 2 | Escape | `0xff1b` | 否 | 无 | 0 | 无 |
| 3 | Return | `0xff0d` | 否 | 无 | 0 | 无 |
| 4 | a | `0x61` | 是 | `a` | 1 | 无 |
| 5 | b | `0x62` | 是 | `ab` | 1 | 无 |
| 6 | c | `0x63` | 是 | `abc` | 1 | 无 |
| 7 | BackSpace | `0xff08` | 是 | `ab` | 1 | 无 |
| 8 | Escape | `0xff1b` | 是 | 无 | 0 | 无 |
| 9 | a | `0x61` | 是 | `a` | 1 | 无 |
| 10 | b | `0x62` | 是 | `ab` | 1 | 无 |
| 11 | c | `0x63` | 是 | `abc` | 1 | 无 |
| 12 | space | `0x0020` | 否 | `abc` | 1 | 无 |
| 13 | A | `0x41` | 否 | `abc` | 1 | 无 |
| 14 | 1 | `0x31` | 否 | `abc` | 1 | 无 |
| 15 | Return | `0xff0d` | 是 | 无 | 0 | **`abc`** |
| — | （不按键，再读一次 commit） | — | — | 无 | 0 | 无 |

矩阵之外还有三项在同一进程内的独立断言：

* **会话隔离**：新会话 B 空 → 在 B 上输入 `x`、`y` → B 的 preedit 为 `xy`，
  同时 A 仍无 preedit、无候选、无提交。
* **无效 schema（双向）**：在会话 C 上 `select_schema("epocanva_no_such_schema_p13")`
  → 记录返回值（实测为 1，见 §7）→ 按 `a` **不得**被消费、**不得**有 preedit/候选/提交，
  且 `get_status().schema_id` 不得等于测试 schema；随后在同一会话上改选真实 schema，
  按 `b` **必须**能组合出 `b`。第二方向是必要的：否则"没有组合"也可能只是这个会话坏了。
* **收尾**：三个 session 逐个 destroy 且 id 不再可解析；`finalize()` 之后
  `create_session()` 必须被拒绝；日志目录必须保持为空。

## 6. 输出字段口径（单位与哨兵值）

```
[replay] ev=06 key=0x0063 'c' mask=0 handled=1  schema=epocanva_test_raw composing=1 disabled=0
         preedit="abc"[len=3 cur=3 sel=0..3] cands=1 cand0="abc" preview="abc" input="abc" commit=-
```

| 字段 | 来源 | 单位 / 合法哨兵 |
| --- | --- | --- |
| `handled` | `process_key()` 返回值 | 1=引擎消费；0=未消费（引擎按 `engine.cc:99-122` 走到 unhandled 分支） |
| `schema` | `get_status().schema_id` | UTF-8 码元（字节）串，由 `rime.dll` 分配、`free_status` 释放 |
| `composing` / `disabled` | `get_status().is_composing/is_disabled` | 布尔 |
| `preedit` | `get_context().composition.preedit` | UTF-8 码元；**哨兵 `-` 表示指针为 NULL**（仅未在组合状态出现） |
| `len/cur/sel` | `composition.length/cursor_pos/sel_start/sel_end` | **对 `preedit` 的字节偏移**，不是字符数、不是屏幕列 |
| `cands` / `cand0` | `menu.num_candidates` / `menu.candidates[0].text` | 个数；字节串 |
| `preview` | `commit_text_preview` | 字节串；本次夹具中与 preedit 相同（记录，不作断言） |
| `input` | `get_input()` | 原始输入缓冲，UTF-8 码元；**无 free 接口**，指针在编辑后失效，程序立即复制且不释放 |
| `commit` | `get_commit().text` | 字节串；**哨兵 `-` 表示 `get_commit` 返回 False**。该调用会取走队列（`rime_api_impl.h:321`），因此第二次读必然为 `-` |

日志只含固定夹具内容，**不读取真实用户输入、剪贴板或宿主文本**；
`process_key` 之外没有任何输入来源。

## 7. 本轮实测到的两条 API 真实性质（必须写进设计约束）

1. **`select_schema()` 对不存在的 schema 也返回 True。**
   `RimeSelectSchema` 只要 session 存在就 `ApplySchema(new Schema(id))` 并返回 True
   （`src/rime_api_impl.h:540-549`），schema 配置加载失败时
   `ConcreteEngine::InitializeComponents()` 在 `if (!config) return;`
   （`src/rime/engine.cc:345-347`）处直接返回，得到一个没有组件的空引擎。
   → **上层绝不能用 `select_schema` 的返回值判断选择是否成功**，必须读回
   `get_status().schema_id` 并用能否组合作为行为学证据。
2. **`deploy_config_file()` 对缺失的源文件也返回 True。**
   `ConfigFileUpdate::Run`（`deployment_tasks.cc:431-452`）在
   `ConfigBuilder::LoadConfig` 因 `resource->loaded == false` 而跳过 `Link()`
   （因而 `SaveOutputPlugin` 不写文件，`config_component.cc:244-253`）之后仍返回 true。
   `deploy_schema()` 则会正确报错，因为 `SchemaUpdate::Run` 显式检查
   `fs::exists(source_path_)`（`deployment_tasks.cc:329-333`）。
   → 判断部署是否真的发生，必须检查 staging 目录里是否出现编译产物。

## 8. 隔离与可重建性

* 运行根目录由 CTest 传入：`build/librime-baseline/replay/rime-input-replay-run/{matrix,missing-fixture}`。
  程序只允许删除**父目录名恰为 `rime-input-replay-run`** 的路径，否则拒绝并返回 6；
  每次运行先删除再重建 `shared/ user/ log/`。
* `shared_data_dir`/`user_data_dir`/`log_dir` 全部指向上述私有目录；
  **不使用 `%APPDATA%\Rime`、不使用个人目录、不全局安装配置**。
* 部署用 `deploy_config_file`/`deploy_schema`，两者都在本进程内同步执行任务
  （`Deployer::RunTask`，`src/rime/deployer.cc:28-40`），**没有维护线程，因此没有 sleep、没有轮询**。
* 已知副作用（都在私有运行目录内，如实登记）：`select_schema()` 会经
  `Switcher::SetActiveSchema`（`src/rime/switcher.cc:165-172`）把
  `var/previously_selected_schema` 与 `var/schema_access_time/*` 写入 `<user>/user.yaml`。
  无效 schema 的探测同样会写入它的 id —— 这正是 §7 第 1 条的可观察痕迹。

## 9. 证明这套测试能失败

结构级（无需重编，实测退出码）：

| 注入 | 预期 | 实测 |
| --- | --- | --- |
| `--expect-dll C:/Windows/not_rime.dll` | 4 | **4** |
| `--run-root` 不在守护目录名下 | 6 | **6** |
| `--fixture-dir` 指向没有夹具 yaml 的目录 | 5 | **5** |
| 缺少必需参数 | 2 | **2** |

值断言级（临时改期望→重编→跑，然后还原）：

| 注入 | 预期 | 实测 |
| --- | --- | --- |
| 事件 4 的期望 preedit 由 `a` 改成 `xx` | 14 | **14**；ctest 报 `***Failed`，并打印 `preedit text: got "a"`、`preedit length: got "1"`、`candidate 0 text: got "a"` |
| 负向模式里把"键不得被消费"改成"必须被消费" | 18 | **18**；`ASSERT FAIL matrix 9 …: got "False"` |
| 以上两处同时注入 | 两个用例都失败 | **`0% tests passed, 2 tests failed out of 2`，ctest 退出码 8** |

还原后重编，两个用例恢复 Passed。

## 10. 阶段码

| 码 | 含义 | 码 | 含义 |
| --- | --- | --- | --- |
| 0 | 通过 | 11 | `get_status()` 失败 |
| 2 | 参数错误 | 12 | `get_context()` 失败 |
| 3 | `rime_get_api()` 返回 NULL | 13 | `get_commit()` 返回非 0/1 |
| 4 | 实际加载模块 ≠ 期望 DLL | 14 | 至少一条值断言失败 |
| 5 | 夹具文件缺失 | 15 | destroy 后 id 仍可解析或返回 False |
| 6 | 运行目录准备失败/守护名不符 | 16 | `get_*`/`free_*` 未配对（泄漏） |
| 7 | traits 读回不符 | 17 | 日志目录出现文件 |
| 8 | 部署返回值或产物不符 | 18 | 负向运行未能复现"失败关闭" |
| 9 | `create_session()` 失败 | 19 | finalize 后仍有 id 可解析 |
| 10 | schema 选择/读回不符（立即中止） | 20 | finalize 后仍能建 session |

任何 API 调用失败都直接返回对应阶段码，**不会用空值继续把测试跑绿**。
