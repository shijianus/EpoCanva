# librime 集成面调查（P1.1）

固定提交：`33e78140250125871856cdc5b42ddc6a5fcd3cd4`（tag `1.17.0`）。
本文只描述**该提交源码中确实存在的东西**，每条都带路径与行号。

> 明确否定的说法：**librime 并没有提供 EpoCanva 英文能力规划所需的全部协议。**
> 它提供的是"分段→翻译→候选→提交"的中文输入流水线与一套 C API；
> 周边文本访问、任意范围替换、已提交文本回改、增量 preedit 区域更新、
> 以及英文形态/纠错所需的多候选-多区间协同，都不在其中（第 8 节逐条列出证据或缺失）。

## 1. 分层结论

```
宿主(TSF/IME)  ←— 我们写的适配层 —→  rime_api.h(C API)  ←— rime_api.cc / rime_api_impl.h —→  C++ 内核
```

- 内核里真正需要英文能力参与的是 **组件层**（`src/rime/gear/`），不是 C API 层。
- C API 是"轮询取快照"的模型（`get_context` / `get_commit`），
  不是"引擎回调宿主做文本编辑"的模型 → 编辑协议必须由我们自己的 C ABI 承担
  （符合 `docs/adr/0002-librime-integration-boundary.md` 的边界决策）。

## 2. 公共 API 与生命周期

| 事项 | 位置 | 说明 |
| --- | --- | --- |
| API 结构体 `RimeApi` | `src/rime_api.h:259` `typedef struct RIME_FLAVORED(rime_api_t)`（注释 `:256-258` 说明它是 v1.0+ 的 API 表） | 纯函数指针表，无 C++ 类型跨边界；首字段 `data_size` 在 `:260` |
| 会话 id 类型 | `src/rime_api.h:57` `typedef uintptr_t RimeSessionId` | 与指针同宽，32/64 位下含义不同，ABI 需固定 |
| 取实例 | `src/rime_api.h:514` `RIME_FLAVORED(rime_get_api)()` | 返回进程内共享实例；`:514` 是唯一入口 |
| 结构体版本协商宏 | `src/rime_api.h:60` `RIME_STRUCT_INIT`、`:62` `RIME_STRUCT_HAS_MEMBER`、`:65` `RIME_STRUCT_CLEAR` | 我们自己的 ABI 可借鉴这套 `data_size` 方案 |
| setup / initialize / finalize | `src/rime_api.h:265` `setup(RimeTraits*)`、`:288` `initialize(RimeTraits*)`、`:289` `finalize()` | 注意 `:248-249` 的 `initialize(void)/finalize(void)` 属于 **`RimeModule`**（`:244-251`），是模块生命周期，不是 API 表的旧版字段 |
| 模块与自定义 API | `src/rime_api.h:240-242` `RimeCustomApi`、`:244-251` `RimeModule`（`get_api` 在 `:250`）、`:253` `RimeRegisterModule`、`:254` `RimeFindModule` | 在不改动核心 API 表的前提下扩展自定义接口的既有机制 |
|  traits（目录、用户 id、log_dir） | `src/rime_api.h:112` `log_dir`、`:113` `prebuilt_data_dir`、`:116` `staging_dir` | **隐私相关**：`log_dir` 一旦开日志就会落盘 |
| 通知回调 | `src/rime_api.h:231` `RimeNotificationHandler`、`:283` `set_notification_handler` | 引擎→宿主只有"消息"，不携带文本编辑指令 |
| 部署 | `:291` `start_maintenance`、`:292` `is_maintenance_mode`、`:293` `join_maintenance_thread`、`:299` `deploy()`、`:300` `deploy_schema` | 重量级异步流程，适配层必须处理"部署中"状态 |
| 用户数据同步 | `:303` `sync_user_data()` | 本地文件操作；是否暴露取决于我们的产品决定 |

## 3. Session、Context 与组合状态

| 事项 | 位置 | 说明与风险 |
| --- | --- | --- |
| 建/找/销毁会话 | `src/rime_api.h:307` `create_session`、`:308` `find_session`、`:309` `destroy_session`、`:310-311` `cleanup_stale_sessions` / `cleanup_all_sessions` | Session 是**独立输入上下文**；跨应用焦点切换只能"销毁 + 新建"或保留 id，没有"迁移上下文"的 API |
| 按键入口 | `:315` `process_key(session_id, int keycode, int mask)` | 键码是 librime 自有编码体系（`src/rime/key_event.cc`），不是 Windows VKEY/Android keycode → 适配层要翻译 |
| 提交组合 | `:317` `commit_composition` | 让引擎把当前组合文本推给 commit 队列 |
| 清空组合 | `:318` `clear_composition` | **没有**"回退已提交文本"的对偶操作 |
| 取快照 | `:322` `get_commit(RimeCommit*)`、`:324` `get_context(RimeContext*)`、`:327` `get_status(RimeStatus*)`，配套 `:323/:326/:329` free | 拉取式；宿主需要自己决定何时刷新 UI |
| 组合状态结构 | `src/rime_api.h:119-125` `RimeComposition { int length; int cursor_pos; int sel_start; int sel_end; char* preedit; }` | **头文件里没有任何单位注释**（见 §3.1） |
| 上下文结构 | `src/rime_api.h:155-163` `RimeContext { data_size; composition; menu; commit_text_preview; select_labels; }` | `data_size` 提供版本协商（`:144` 注释要求用 `RIME_STRUCT_INIT`） |
| 菜单结构 | `src/rime_api.h:133-141` `RimeMenu { page_size; page_no; is_last_page; highlighted_candidate_index; num_candidates; candidates; select_keys; }` | 页模型，不是"全量候选列表" |
| 候选项 | `src/rime_api.h:127-131` `RimeCandidate { char* text; char* comment; void* reserved; }` | 只有文本 + 注释，**没有区间信息** |
| 内核侧上下文 | `src/rime/context.h:34` `GetPreedit()`、`:35` `IsComposing()`、`:63-64` `set_input/input()`、`:66-67` `set_caret_pos/caret_pos()`、`:69-71` `composition()`、`:51` `DeleteCandidate(size_t)`、`:102` `size_t caret_pos_` | `caret_pos_` 是 `size_t`，即**输入串内偏移** |
| 组合的内部模型 | `src/rime/composition.h:14-19` `struct Preedit { string text; size_t caret_pos; size_t sel_start; size_t sel_end; }` | 这三个 `size_t` 是**UTF-8 字节偏移**，见 §3.1 |

### 3.1 单位与哨兵值（必须在自有 ABI 里先解决的问题）

源码链路证据：

1. `src/rime_api_impl.h:222` `context->composition.length = preedit.text.length();`
   → `std::string::length()` = **UTF-8 码元（字节）数**。
2. `src/rime_api_impl.h:225-227` 把 `preedit.caret_pos / sel_start / sel_end` 直接赋给 `int` 字段。
3. 这三个值在 `src/rime/composition.cc:34`（`preedit.caret_pos = preedit.text.length()`）、
   `:48-49`（`sel_start = text.length()`；`sel_end = string::npos`）与
   `:69-70`（`sel_end = text.length()`）处赋值 → 全部是**同一个 preedit 串内的字节偏移**。
4. **哨兵值会被截断**：`string::npos`（`size_t` 全 1）写入 C 结构的 `int` 字段。
   在 x64 上得到 `-1`。头文件未说明 `-1` 的含义 → 适配层必须把"负值"当作合法哨兵处理，
   不能假设它一定存在或一定是 -1（**未验证**：需要实测哪些状态会留下 npos）。
5. **preedit 串里可能被插入光标占位符**：`composition.cc:84-93`
   （`preedit.text.insert(preedit.caret_pos, prompt)`，并按 `prompt.length()` 调整 `sel_start/sel_end`）
   → 宿主拿到的 `preedit` 字符串**可能包含一个软光标字形与 prompt 文本**。
   若把它整段画进 TSF composition，就会把不可见的编辑标记显示出来，并让字节偏移与实际文本错位。
6. 结论：**Windows 侧 UTF-16 代码单元偏移与 librime 的 UTF-8 字节偏移必须在适配层显式换算**，
   且换算前必须先剥离/识别 caret 占位符。这条正是 `AGENTS.md` 第 5 条要求先定死的东西。

## 4. 四类组件的职责（接口真实签名）

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| `Processor` | `src/rime/processor.h:18` `enum ProcessResult { kRejected, kAccepted, kNoop }`；`:24` `class Processor : public Class<Processor, const Ticket&>`；`:30` `virtual ProcessResult ProcessKeyEvent(const KeyEvent&)` | 按键处理。`kRejected`=交回系统，`kAccepted`=消费，`kNoop`=留给后续 processor（注释在 `:19-21`） |
| `Segmentor` | `src/rime/segmentor.h:18`，`:24` `virtual bool Proceed(Segmentation*) = 0` | 决定"从当前位置吃掉多少输入"，即切分边界；不产出候选 |
| `Translator` | `src/rime/translator.h:22`，`:28-29` `virtual an<Translation> Query(const string& input, const Segment& segment) = 0` | 由一段输入产出候选流（`an<>` = `boost::shared_ptr`，见 `src/rime/common.h`） |
| `Filter` | `src/rime/filter.h:22`，`:28-29` `virtual an<Translation> Apply(an<Translation>, CandidateList*) = 0`；`:31` `virtual bool AppliesToSegment(Segment*)` | 对已有候选流做变换/去重/改写/追加 |

共同的注册与查找机制：

- `src/rime/component.h:20-31`：`Class<T,Arg>::Component::Create(Initializer)` 抽象工厂 +
  `Class<T,Arg>::Require(name)` 通过 `dynamic_cast` 从 Registry 查名字。
- `src/rime/component.h:33-37`：`Component<T>` 是通用实现（`new T(arg)`）。
- `src/rime/registry.h:21-22`：`Registry::Find(name)` / `Registry::Register(name, ComponentBase*)` —
  **公开接口，可在我们自己的代码里调用，无需修改上游**。
- 上游组件注册写法示例：`src/rime/gear/gears_module.cc:39` `rime_gears_initialize()`，
  `:43` 取 `Registry::instance()`，`:46-90` 逐个 `r.Register("ascii_composer", new Component<AsciiComposer>)` 等，
  `:95` `RIME_REGISTER_MODULE(gears)`。

## 5. 候选与提交的数据流（真实调用链）

全部在 `src/rime/engine.cc`（类 `ConcreteEngine`，声明在 `:32-35`）：

1. `ProcessKey` `:99` — 依次调用 `processors_`（循环在 `:102`），遇 `kRejected` 停止、`kAccepted` 返回（`:103-108`）；
   未被消费的按键记入 `context_->commit_history().Push(key_event)`（`:110`）；
   再跑 `post_processors_`（`:112-118`）；最后触发 `unhandled_key_notifier`（`:120`）。
2. 状态变化回调 `OnContextUpdate` `:124` → `Compose(ctx)` `:127`。
3. `Compose` `:154` — 以 `ctx->input().substr(0, ctx->caret_pos())` 作为"活动输入"重建 composition（`:156-158`），
   光标不在末尾时扩展到整串（`:160-164`），然后 `CalculateSegmentation` + `TranslateSegments`（`:166-167`）。
4. `CalculateSegmentation` `:171` — 循环调用 `segmentors_`，逐个 `Proceed(segments)`（`:181` 附近）；
   **没有推进就 break**（`:190-191`），`start_pos >= caret_pos` 时停止（`:193-194`）。
5. `TranslateSegments` `:203` — 对每个尚未 `kGuess` 的 segment（跳过判断在 `:208`）：
   `size_t len = segment.end - segment.start`（`:210`）与
   `segments->input().substr(segment.start, len)`（`:211`）证明
   **`Segment::start/end` 是输入串的 UTF-8 字节区间**；
   新建 `Menu`（`:213`），依次让 `translators_` `Query`（`:215`），再让 `filters_` `Apply`（`:225`）。
6. 提交：`CommitText` `:244` → `sink_(text)`（`src/rime/engine.h:27`，`CommitSink = signal<void(const string&)>` `:22`）；
   `OnCommit` `:251` 取 `ctx->GetCommitText()`。C API 侧对应 `get_commit`（`src/rime_api.h:322`）。
7. 组件列表来源：`InitializeComponents` `:328` 清空四类列表（`:329-333`），
   再由 schema 配置的名字列表创建：`:350-351` `engine/processors`、`:352-353` `engine/segmentors`、
   `:354-355` `engine/translators`、`:356-357` `engine/filters`（模板 `CreateComponentsFromList<T>`）。
   Formatter/ post-processor 是硬编码名字：`:359` `Formatter::Require("shape_formatter")`、
   `:367` `Processor::Require("shape_processor")`。
8. 候选操作（C API）：`select_candidate` `src/rime_api.h:436`、
   `select_candidate_on_current_page` `:445`、`candidate_list_begin/next/end` `:449-452`、
   `candidate_list_from_index` `:458-459`、`delete_candidate` `:483`、`highlight_candidate` `:502`。

## 6. 英文能力可以挂在哪里（不需要改上游）

| 规划的英文模块（`docs/architecture/overview.md` §2.5） | 可挂载点（真实证据） | 说明 |
| --- | --- | --- |
| `english-tokenizer` / `english-segmenter` | `Segmentor::Proceed`（`segmentor.h:24`）；参考现有 `abc_segmentor`（`gears_module.cc:60`）、`speller`（`:56`） | 由 Latin 输入决定 segment 边界 |
| `english-ranker` / 词候选 | `Translator::Query`（`translator.h:28`）；现有 `script_translator`（`:71`）、`table_translator`（`:70`）作对照 | 返回 `Translation` 流即可进入同一 menu |
| `english-morphology` / `english-corrector` | `Filter::Apply`（`filter.h:28`）；现有 `uniquifier`（`:81`）、`simplifier`（`:80`）、`single_char_filter`（`:87`）、`charset_filter`（`:83`）为同型实现 | 对候选流做改写/派生形式/纠错替换 |
| 空格选择、退格回退、标点提交英文 | `Processor::ProcessKeyEvent`（`processor.h:30`）；对照 `express_editor`/`fluid_editor`（`:48-50`）、`punctuator`（`:53`）、`selector`（`:55`） | 决定何时把候选 commit |
| 上屏后下一词预测 | 现有 `history_translator`（`:77`）、`contextual_translation`（`src/rime/gear/contextual_translation.h`）+ `grammar.h`；真正的语言模型在**外部插件**（octagram/predict，本阶段不引入） | 核心里只有接入点，没有模型 |
| 注册方式 | `Registry::Register`（`registry.h:22`）由我们自己的模块在 `initialize` 阶段调用 | **无需上游补丁**；schema 里用名字引用 |
| 跨 schema/跨应用一致性 | `set_option` / `get_option`（`src/rime_api.h:333-334`）、`set_property` / `get_property`（`:336-339`） | 可用于关闭/开启英文行为，但**不含**用户级学习状态查询接口 |

## 7. 需要在下一步用实验确认的假设

| # | 假设（当前只有源码依据，未实测） | 验证方法 |
| --- | --- | --- |
| 1 | `RimeComposition` 的 `cursor_pos/sel_start/sel_end` 是 UTF-8 字节偏移，且在部分状态下为 `-1`（npos 截断） | 构造含 CJK/emoji 的 preedit，读回三值并与 `strlen(preedit)` 对照 |
| 2 | preedit 串可能内含软光标/占位符（schema 的 `caret` 设置） | 用带 prompt 的 schema 观察 `preedit` 是否比可见文本长 |
| 3 | 在**不改上游**前提下，我们自己的模块注册 + schema `engine/translators` 追加名字即可插入英文 Translator | 做一个最小自研 Translator 并观察候选是否进入同一 menu |
| 4 | `ConcreteEngine::active_engine()`（`engine.h:34-35`）可用于把英文组合"委托"给辅助引擎 | 读 `ApplySchema`/`set_active_engine` 使用点并实测消息路由 |
| 5 | `Filter::Apply` 能否**重排/替换**已确认 segment 的候选（英文自动纠正所需） | 用 `uniquifier` 同型实现做替换实验 |
| 6 | `menu` 分页与 `highlighted_candidate_index` 是否足以表达"逐词候选 + 内联替换" | 观察 `candidate_list_from_index`（`:458`）行为 |
| 7 | `ENABLE_LOGGING=OFF` 时 `LOG(INFO)` 全部走 `src/rime/no_logging.h`，无残留符号依赖 | 实际构建一次（第 1 个必须做的实验） |
| 8 | `data_size` + `RIME_STRUCT_HAS_MEMBER`（`src/rime_api.h:62`）能否保证我们只读新增字段时不越界 | 写一个只声明旧字段的结构体做实验 |

## 8. 现有 API 满足不了的（必须自有 ABI/适配层解决）

| 需求 | 现有 API 是否提供 | 证据 |
| --- | --- | --- |
| 读取宿主**周边文本**（光标前后若干字） | ❌ 不提供。librime 只有自己的 `commit_history`（`src/rime/commit_history.h`），记录的是**引擎自己**提交过的项（`engine.cc:245`、`:252`）与未处理按键（`:110`），**没有**宿主缓冲区视图 | `src/rime_api.h` 内不存在任何读取外部文本的入口 |
| **任意范围替换**（替换文档中已存在的文本） | ❌ 不提供。API 只输出"要提交的文本"（`get_commit` `:322` / `CommitText` `engine.cc:244`），区间由宿主决定 | 同上 |
| **已提交文本的回改**（commit 后再改成别的词，英文自动纠错刚需） | ❌ 不提供对偶操作；`commit_composition`（`:317`）与 `clear_composition`（`:318`）都不返回"已提交区间" | API 表 `:317-318` |
| 精确的 preedit **屏幕列**（IME candidate window 定位） | ❌ 不提供。只有输入串内的字节偏移（§3.1）；无渲染宽度信息 | `composition.h:14-19` |
| 增量 UI 更新事件（只告诉宿主"第 x..y 区间变了"） | ⚠️ 只有粗粒度通知（`RimeNotificationHandler` `:231`）；宿主需要自己 diff `get_context` 快照 | `:283` |
| 多光标/多区间协同（英文"一词多形"同时改写多处） | ❌ 无对应概念 | 内核模型是单串 + 单 caret（`context.h:66-67,102`） |
| 学习状态的**可查询/可清除**接口（隐私必需） | ⚠️ 有 `delete_candidate`（`:483`）与 `sync_user_data`（`:303`），但没有"清空学习/查看学习内容"的 API | API 表内无相应函数 |
| 字段隐私等级（密码框等） | ❌ librime 完全不感知宿主字段类型，必须我们自己在适配层识别并阻断学习 | C API 无相关字段（`RimeTraits` `:112-116` 只有目录信息） |

这八条就是 `docs/architecture/overview.md` §2.2/§2.3 里"自有 C ABI 与 Session/编辑协议必须自己定义"的**具体理由**，
而不是抽象主张。英文能力越依赖"回改 + 周边文本 + 范围替换"，就越不能假设 librime 会替我们做。

## 9. 本阶段未做

未设计自有 ABI、未实现任何组件、未编译 librime、未引入插件、未引入词库、
未把英文降格成一个 Translator + 一个词典、未改成与 librime 无关的另一套架构。
第 7 节的 8 条假设**全部未验证**，是下一步实验清单，不是结论。
