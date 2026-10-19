# `tests/fixtures/rime_probe/` — 编译进引擎的自有组件可达性夹具（P1.4）

消费方：`tools/rime_component_probe`。
运行方法与判定口径见 `docs/testing/rime-component-probe.md`；
阶段结论见 `docs/reports/P1.4-component-probe.md`。

本夹具与 `tests/fixtures/rime_raw/`（P1.3）**互不覆盖**：
`rime_raw` 只用上游已注册组件，仍是 baseline 引擎的回归基线；
本目录多出的唯一一项是自有组件 `epocanva_probe_processor`，
它在 baseline 引擎里**不存在**，因此本夹具在 baseline 上只能作为负向输入使用。

---

## 1. 这个夹具证明什么，不证明什么

**证明**：把 EpoCanva 自有源码编译进 `rime.dll` 之后，
该 DLL 里确实存在一个自有模块、该模块被真实机制装载、
它的 `Processor` 被构造、按键确实进入它的 `ProcessKeyEvent()`、
它对一个指定测试键返回"已消费"，并且引擎销毁后实例被释放。

**不证明**（每一项都是"未实现"，不得写成已通过）：

| 项 | 状态 |
| --- | --- |
| 英文补全、预测、词频学习、形态变化、纠错、自动分段、缩写展开 | **未实现**。`AGENTS.md` §1 的全部要求本阶段完全未触及 |
| 任何语言学行为 | 候选文本与按键**逐字节相同**（`echo_translator` 原样回显） |
| Unicode / 非 ASCII / 字素簇 / 屏幕列换算 | **未验证**。夹具数据全为 ASCII |
| 词典、用户词库、学习数据 | **不使用**（无 `translator/dictionary`） |
| 生产可用的自有组件 | 探针组件的职责是**可达性见证**，不是功能 |

## 2. 三个文件

| 文件 | 作用 |
| --- | --- |
| `default.yaml` | 单条 `schema_list`（只指向 `epocanva_test_probe`）+ `menu/page_size` |
| `epocanva_test_probe.schema.yaml` | 在 `rime_raw` 的组件集合之上，把 `epocanva_probe_processor` 放在 `engine/processors` **第一位** |
| `README.md` | 本文件：选型理由、测试键契约、单位、"不是什么" |

## 3. 组件选型（逐项读自固定提交，不是猜的配置名）

上游 5 个组件的理由与 `tests/fixtures/rime_raw/README.md` 完全相同
（`speller` / `fluid_editor` / `abc_segmentor` / `fallback_segmentor` /
`echo_translator`，注册名见 `src/rime/gear/gears_module.cc:46-88`），
本轮只增加一条，并解释"为什么放在第一位"。

| 角色 | 组件 | 依据 |
| --- | --- | --- |
| 自有见证 | `epocanva_probe_processor` | 注册发生在 `engine/rime_components/probe/probe_module.cc`；`Registry::Register` 的用法照抄 `gears_module.cc:44-57` 的真实机制 |
| 为什么排第一 | 处理器链按列表顺序遍历，第一个返回 `kAccepted` 者胜出，其余不再看到该键 | `src/rime/engine.cc:99-104`（`ConcreteEngine::ProcessKey`）、`src/rime/engine.cc:337, 350`（`Switcher` 先入列，然后是配置顺序） |
| 为什么仍能保持原链路 | 探针除裸 F6 之外一律返回 `kNoop` | `engine/rime_components/probe/probe_processor.cc`；`ProcessResult` 语义见 `src/rime/processor.h:18-22` |
| `Switcher` 是否干扰 | 不干扰：夹具无 `switcher/hotkeys`，`hotkeys_` 空表且 `active_` 为 false → 对任何键 `kNoop` | `src/rime/switcher.cc:51-81`、`src/rime/engine.cc:337` |
| `shape_processor` 是否干扰 | 不干扰：`full_shape` 未开时立即 `return kNoop`；若被开启会把未处理 ASCII 转全角**直接提交**，因此夹具不开任何 switch | `src/rime/engine.cc:359-373`、`src/rime/gear/shape.cc:38-42, 51-54` |

配套的两处关闭动作沿用 `rime_raw`：`editor/char_handler: noop`
（清掉 `AddToInput`，`src/rime/gear/editor.cc:34-39, 74-85, 201`）、
`editor/bindings: {space: noop}`（抹掉 `{XK_space,0} → Confirm`，
`src/rime/gear/key_binding_processor_impl.h:71-78`）。
**本夹具不新增任何标点、热键或提交键**，提交键仍然只有 Return。

## 4. 测试键契约（单位与取值）

| 项 | 值 | 出处 |
| --- | --- | --- |
| 键 | **F6** | `third_party/librime/include/X11/keysymdef.h:257` → `#define XK_F6 0xffc3` |
| 不是 | `0xffbe` 是 **XK_F1**（`:252`） | 任务书允许"优先核实 F6"，核实结果就是 F6 可用，但值必须按头文件取 |
| 修饰键 | `mask = 0`（无 Shift/Control/Alt） | 修饰位定义 `src/rime/key_table.h:15-43`（bit0 Shift、bit1 Lock、bit2 Control、bit3 Mod1/Alt、bit30 Release） |
| 匹配方式 | `keycode() == 0xffc3 && modifier() == 0` | `src/rime/key_event.h:24-27` |
| 命中后的动作 | 计数 +1，返回 `kAccepted`；**不调用任何 Context/Engine 方法** | `engine/rime_components/probe/probe_processor.cc` |
| 未命中的动作 | 计数 +1，返回 `kNoop`，链路照旧 | 同上 |

负向用例另外使用 **Shift+F6（keysym 0xffc3，mask 1）**：
探针**必须不**消费它，因此它落到链路末尾成为 unhandled（`handled=0`）。
这条断言的作用是防止"探针吞掉一切键"也能把测试跑绿。

## 5. 单位声明（`AGENTS.md` §5）

- 跨 C ABI 的字符串一律是 **UTF-8 码元（字节）**；`buffer_size` 数的是字节。
- `RimeComposition.length/cursor_pos/sel_start/sel_end` 是**对 `preedit` 的字节偏移**，
  不是字符数、不是 UTF-16 码元、不是屏幕列。
- 诊断计数器是**事件计数**，不是长度或偏移。
- keysym 是 32 位整数标识符，不是字符编码；`mask` 是 librime 自己的修饰位。
- 本夹具数据全为 ASCII，所以"字节 = UTF-8 码元 = 字素簇"在这组数据上成立；
  这是**测试数据的性质**，不是对引擎的断言，也不得写成对英文输入的保证。

## 6. 隐私与离线

- 夹具与探针**都不读写任何生产词库、用户目录或个人 Rime 配置**。
  运行目录由测试程序在 `build/` 下私有重建，见 testing 文档 §5。
- 探针组件**不记录输入内容**：没有键码表、没有组合缓冲、没有候选文本被存进任何
  计数器（`probe_diagnostics.h` 头部逐条声明）。计数只回答"跑过几次"。
- 组件不发起任何网络 I/O；`rime.dll` 本身在这套构建下也不做网络访问
  （`docs/adr/0003-runtime-offline-policy.md`）。
- 构建产物 `build/` 整棵树仍因词库许可未清审而**禁止对外发布**
  （`docs/reports/P1.3-input-replay.md` §9 第 5 项）。本夹具文件本身来自
  `tests/fixtures/`，与上游 `data/minimal/` 无关。

## 7. 负向运行怎么用这份夹具

| 负向项 | 做法 | 位置 |
| --- | --- | --- |
| 用**原版** baseline `rime.dll` 跑本夹具 | 探针模块不存在，`find_module()` 必须返回 NULL，F6 必须不被消费 | 第二个可执行文件 `rime_component_probe_baseline.exe`，链接 baseline 的 `rime.lib` |
| 用**自有** DLL，但从 schema 里删掉探针组件 | 模块仍在 DLL 里，但组件构造计数必须为 0，F6 必须不被消费 | 测试程序把去掉那一行的副本写进**自己的运行目录**，绝不回写 `tests/fixtures/`（`--drop-probe-component`） |

两条都是**正向断言"引擎没有这个能力"**，因此它们各自返回 0 才算通过；
不需要 `WILL_FAIL`，也不能靠 `PASS_REGULAR_EXPRESSION` 变绿。
