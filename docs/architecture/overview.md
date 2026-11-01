# EpoCanva 架构总览

> **这是规划文档。** 下列分层、模块与协议在本仓库中**尚未实现**。
> 本文件描述的是目标形态，不是当前实现清单；每一行的真实状态只以 §5 为准，
> 而阶段结论以 `docs/reports/` 与 `README.md` 的阶段状态表为准。
> 文中出现的模块名是未来实现的命名约定，不代表对应代码已存在。

## 1. 目标与约束来源

架构必须同时满足 `docs/product/requirements.md` 中的：固定语言优先级
（English > Chinese > French > Japanese > Korean > 通用 i18n）、librime 作为可改造底座、
运行期完全离线、以及 Windows/Android 权限模型不对称这三条硬约束。

## 2. 分层

```
┌──────────────────────────┬───────────────────────────┐
│  platform-win/tsf        │  platform-android/ime     │   平台适配层
│  (TSF COM 组件, UTF-16)  │  (InputMethodService)     │
└────────────┬─────────────┴────────────┬──────────────┘
             │   epocanva_engine.h  (自有 C ABI, UTF-8 + 显式单位)
┌────────────┴──────────────────────────┴──────────────┐
│  engine-core        Session / 编辑协议 / 候选编排      │   引擎门面
├─────────────────────────────────────────────────────┤
│  english-*          英文能力模块（非词库配置项）        │   能力层
│  rime-adapter       librime 生命周期与线程模型封装      │
├─────────────────────────────────────────────────────┤
│  data-store         用户词库 / 学习状态 / 同步与备份     │   数据层
└─────────────────────────────────────────────────────┘
```

### 2.1 平台适配层

职责：把宿主应用的文本接口翻译成引擎语义，不承载任何语言学决策。

- `platform-win/tsf`：实现 TSF 的 `ITfTextInput`/`ITfComposition`/
  `ITfTextEditSource` 一类接口，向宿主提供 preedit 绘制与提交、UI 回调、
  以及宿主焦点与字段属性（密码字段等）读取。语言：UTF-16。
- `platform-android/ime`：`InputMethodService` 封装，负责候选窗口、
  提交、以及 `InputConnection` 的 `setComposingText`/`commitText` 语义。
- 两者**都不得**包含候选排序、语言判断、学习逻辑；两者**都不得**直接调用 librime。
- 字段隐私等级由适配层识别（TSF `TATTE_*` / Android `imeOptions`、
  `inputType` 的 visible-password 标志），并作为枚举传给引擎；
  适配层不上传字段名或窗口标题（见威胁模型）。

### 2.2 自有 C ABI（`epocanva_engine.h`，未实现）

存在理由：librime 的 C++ 类型、DLL 导出面与内存所有权约定不适合直接暴露给
两个差异极大的平台层。自有 ABI 提供稳定边界，使 librime 可被替换或源码级改造
而不触动适配层。

约定（未来实现时逐条固化）：

- `extern "C"`，不跨边界抛异常，不跨边界 new/delete，结构体带 `size` 字段做版本协商。
- 文本一律 UTF-8；偏移必须带显式单位，ABI 文档禁止裸 `int index`。
- 句柄式 `epocanva_session_t*`，所有权在 ABI 文档中逐函数说明。
- ABI 变更需 ADR，且需提供兼容性检查用例。

### 2.3 Session / 编辑协议（未实现）

一个 Session 表示"一次从按下第一键到上屏或取消的输入过程"，状态机至少需要区分：
`idle / composing / candidate-shown / committed / reverted / cancelled`。

编辑协议必须回答清楚（这是当前规划里最容易出错、必须最早定下的部分）：

| 概念 | 定义要求 |
| --- | --- |
| preedit 区间 | `[start, end)` + **单位**（UTF-16 代码单元，或 UTF-8 字节，或字素簇） |
| caret 锚点 | preedit 内部光标位置 + 同一单位声明 |
| commit | 一次性文本 + 其上屏位置 + 单位 |
| 回改（revert） | 已 commit 文本被重新取回编辑的前提与失败降级 |
| 选择/替换 | 宿主存在选区时的行为（替换 / 追加 / 放弃组合） |
| 外部修改 | 宿主在组合期间自行改文本时的检测与恢复策略 |
| 跨字段 | 焦点切换时 Session 是丢弃、迁移还是提交 |

Windows（UTF-16）与 librime（UTF-8）的单位转换**只允许**发生在适配层，
并在转换处配对抗性测试（代理对、组合字符、ZWJ emoji、CJK 扩展区）。

### 2.4 rime-adapter（未实现）

- 唯一被允许 `#include <rime/...>` 并链接 librime 的模块。
- 负责：初始化、部署、线程与消息回调、Schema/Option 管理、
  把 librime 的组合/候选模型映射到自有 Session/编辑协议。
- 源码级改造通过 `third_party/patches/` 施加，见 `third_party/README.md`；
  适配层负责隔离补丁带来的行为差异。
- librime 的"部署（deployment）/用户目录同步"是重量级操作，
  其触发时机与失败恢复策略必须由该层显式定义，而不是继承自参考实现的默认行为。
- 上游实际接口与挂载点已按固定提交调查完毕，结论见
  `docs/architecture/librime-integration-map.md`（含"现有 API 满足不了的 8 项"）；
  构建形态见 `docs/build/librime-windows-baseline.md`，依赖与许可证见
  `docs/compliance/dependency-audit.md`。

### 2.5 英文能力模块（部分实现）

独立于 rime-adapter 的能力层，**不以"一个英文 Translator + 一个词库"为交付**。
规划中的子组件（名称为占位约定）与当前实际状态：

已存在（`engine/languages/en/`，不含任何 Rime 或平台头，因此可被根工程直接单测）：

- `english-tokenizer` → **部分**：`english_token.{h,cc}` 判定**单个 token** 的形状
  （字母串、至多一个 ASCII 分隔符、停止字节类别）。它**不**把连续输入流切成多个词——
  词与词之间的边界目前由用户的空格键决定（见下），不是切分算法。
- 词表与查询：`english_lexicon.{h,cc}` + `prefix_completer.{h,cc}`（P2.1）。
- ASCII 大小写：`english_case.{h,cc}`（P2.2）。
- **空格与提交边界**：`english_space_policy.{h,cc}`（P2.5）。它只回答两件事——
  这次按键事件是不是"用户要一个空格"（Ctrl/Alt/Super/release 都不是），
  以及当前组合状态落到哪条规则（A 提交原文 + 空格 / B 提交已选词 + 空格 / C 只空格）。
  决策与副作用分离：规则在这个纯函数层，执行在编译进 `rime.dll` 的
  `engine/rime_components/english/english_space_processor.{h,cc}`；
  后者**不保存任何文本状态**（无成员数据），所以"重复提交上一个词"这类故障在结构上
  就不可能出现，session 之间也不存在共享状态。

未实现：

- `english-morphology`：词形生成/归一（复数、时态、所有格、缩写还原）。
- `english-ranker`：结合词频、上下文、个人学习信号的候选排序。
  （P2.4 试验词表的权重是"被几部源词典收录"的粗分层，**不是**语料词频。）
- `english-corrector`：拼写纠错与键位邻近容错。
- `english-segmenter`：连写切分与短语还原（`helloworld` 目前整串保留）。
- `english-predictor`：上屏后下一词预测。

与中文路径的关系：共享 Session 与候选编排框架，不共享语言学假设。
其他语言（法/日/韩）在英文与中文之后复用同一能力层接口，不另起架构。

### 2.6 数据与学习存储（未实现）

- 用户词组、学习计数、个人偏好、同步元数据。
- 必须支持：隐私字段黑名单在**写入侧**生效（而非读取侧过滤）；
  导出/备份的显式用户动作；本地删除语义（见威胁模型第 6 节）。
- 学习信号不得包含可还原的原始输入序列。
- 该层的格式版本化与迁移策略属于数据层，不下沉到平台适配层。

## 3. 依赖方向

```
platform-*  →  epocanva_engine.h(ABI)  →  engine-core  →  {english-*, rime-adapter, data-store}
rime-adapter → librime (third_party, 仅此处可见)
```

禁止：`platform-* → librime`、`data-store → platform-*`、任何反向依赖、
以及跨层共享 librime 类型。

## 4. 与参考实现的关系

小狼毫（Weasel）与同文（Trime）作为 librime 平台集成的**参考资料**阅读，
用于核对既有行为；EpoCanva 不采用其外壳、不沿用其 ABI、不以其配置结构作为自身边界。
换皮发行违反 `docs/product/requirements.md` 第 1 节。

## 5. 当前实现状态

| 规划模块 | 状态 |
| --- | --- |
| toolchain probe | **已实现**（P0.1，仅工具链/字节/摘要自检） |
| CTest 基础设施 | **已实现**（4 条用例，运行真实探针） |
| librime 引入与构建 | 上游已锁定并审查（P1.1：librime 1.17.0 @ `33e7814…`，见 `third_party/sources.lock.json`）；**已构建两套引擎**：原版 baseline（P1.2）与把自有组件编译进去的 component-probe（P1.4），均为 Release/x64/静态依赖/共享 `rime.dll`/`/MT` |
| 自有 rime 组件 | **已实现一个测试探针**（P1.4，`engine/rime_components/probe/`）：可达性见证，不含任何语言能力；接入形态见 `docs/adr/0004-compiled-in-rime-components.md` |
| 无界面测试前端 | **已实现**（P1.2 生命周期冒烟、P1.3 按键回放、P1.4 组件可达性），全部只使用公开 C API |
| epocanva_engine.h (C ABI) | 未实现 |
| platform-win/tsf | **W3-B 已实现注册导出**：`platforms/windows/tsf/` 导出恰好 `DllGetClassObject`/`DllCanUnloadNow`/`DllRegisterServer`/`DllUnregisterServer`，注册策略支持 HKLM/HKCU、逐项回读、失败回滚、外来 CLSID 拒绝；**本机尚未执行系统注册**（安装器工具链缺失，C 层闭环待授权）。产品 TSF 层（W1–W3-A）已有按键 sink、候选 UI、宿主文本编辑与引擎接入；C ABI 单位换算的适配层函数已落地但尚未接到 ABI 边界与会话上。构建与验证见 `docs/windows/tsf-bootstrap.md` 与 `docs/reports/W3-B-install-and-fallback.md` |
| platform-win/text | **已实现**（P3.2，落地目录 `platforms/windows/text/`）：严格 UTF-8 ↔ UTF-16 转换（只经 `MultiByteToWideChar` / `WideCharToMultiByte` + `CP_UTF8` + `*_ERR_INVALID_CHARS`，先查询容量再核对写入长度），以及一份与**同一份不可变文本**绑定的 UTF-8 字节偏移 ↔ UTF-16 码元偏移边界表；越界、序列内部、代理对内部偏移一律拒绝，不取整不 clamp。规范化、字素簇、大小写、文本身份/版本校验**未实现**；契约与验证见 `docs/windows/text-boundary-contract.md` |
| platform-android/ime | 未实现 |
| engine-core / Session 协议 | 未实现 |
| rime-adapter（本项目的引擎封装层） | 未实现——现有 `engine/rime_components/` 只是编译进引擎的组件，不是封装层 |
| english-* 能力模块 | **部分实现**：`engine/languages/en/` 有词表加载与前缀查询（P2.1）、ASCII 大小写映射（P2.2）、词形与受保护文本判定（P2.3）、空格与提交边界判定（P2.5），均以编译进 `rime.dll` 的自有组件形态存在（P2.1/P2.5）。规划中的 `english-morphology`、`english-ranker`、`english-corrector`、`english-segmenter`、`english-predictor` **未实现** |
| data-store / 学习 | 未实现 |
| 安装包与签名 | 未实现 |
