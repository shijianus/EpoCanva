# EpoCanva 架构总览

> 本文件同时描述**目标分层**与**当前真实状态**。§2 说明目标形态，§5 是逐模块的
> 证据状态表；两者冲突时以 §5 与 `docs/reports/` 为准。
>
> 上一版曾整篇写"下列分层尚未实现"，但其中 C ABI、Session、学习存储、语言能力模块
> 早已落地。那种写法会让协作者按错误前提工作，因此本轮重写。

## 1. 目标与约束来源

架构必须同时满足 `docs/product/requirements.md` 中的：

- 固定语言优先级 `English > Chinese > French > Spanish > Japanese > Korean > German`
  （机器可读实现：`engine/locale/language_tag.cc` 的 `LanguagePriority`）；
- librime 作为可改造底座；
- 运行期完全离线；
- 不装神经模型也能完成基础打字与候选排序；
- Windows / Android 权限模型不对称。

## 2. 分层

```
┌──────────────────────────┬───────────────────────────┐
│  platforms/windows/*     │  (future) android/ime     │   平台适配层
│  TSF COM 组件, UTF-16    │  InputMethodService       │
└────────────┬─────────────┴────────────┬──────────────┘
             │  epocanva_engine.h / epocanva_backend.h
             │  （自有 C ABI，UTF-8 + 显式单位，带 struct_size 版本）
┌────────────┴──────────────────────────┴──────────────┐
│  engine core / backend session                       │   引擎门面
│  Session、候选快照、选择、提交与 ACK 协议             │
├─────────────────────────────────────────────────────┤
│  engine/locale         语言/文字/地区/方案身份       │   身份层
│  engine/language_packages  包清单与能力准入           │
│  engine/languages/{en,zh,fr,ja,ko}  语言能力模块      │
│  engine/lexical_service  跨语言词法候选               │
│  engine/prediction       n-gram 与缩写解码            │
│  engine/ranking          自适应排序与学习事件语义      │   排序层
│  engine/data_store       本地学习存储（ID/计数）       │   数据层
│  engine/neural           可选神经重排（未接产品）      │   可选层
│  engine/adapter + rime_components  librime 边界       │
└─────────────────────────────────────────────────────┘
```

依赖方向（仍然有效）：

```
platform-*  →  C ABI  →  engine core/backend  →  {locale, languages/*, lexical_service,
                                                    prediction, ranking, data_store, adapter}
engine/adapter → librime（仅此处可见）
```

禁止：`platform-* → librime`、`data_store → platform-*`、任何反向依赖、
跨层共享 librime 类型。语言算法层（`locale`、`languages/*`、`ranking`）不得包含
Windows 头文件——这是 Android 复用的前提。

### 2.1 平台适配层

职责：把宿主文本接口翻译成引擎语义，不承载语言学决策。

- `platforms/windows/tsf`：TSF COM 组件，UTF-16。负责按键准入、上下文观察、
  字段权限、候选 UI、编辑会话与卸载生命周期。
- `platforms/windows/tsf_product`：产品级控制器，协调上述组件与统一后端。
- `platforms/windows/text`：严格 UTF-8 ↔ UTF-16 转换与边界表。
- 未来 `android/ime`：`InputMethodService`，`setComposingText`/`commitText`。
- 两者**都不**包含候选排序、语言判断或学习逻辑；**都不**直接调用 librime。

### 2.2 自有 C ABI

已存在，不是规划：

- `engine/include/epocanva_engine.h`（`EPOCANVA_ENGINE_ABI_VERSION 1`）：
  引擎打开/关闭、会话、按键、候选、提交、信息查询。
- `engine/include/epocanva_backend.h`（`EPOCANVA_BACKEND_ABI_VERSION 1`，
  追加式 profile 扩展 `EPOCANVA_BACKEND_PROFILE_ABI_VERSION 2`）：
  多资源后端、profile 会话、UTF-8 输入、上下文导入、确认与 ACK。
- 约定：`extern "C"`；不跨边界抛异常；不跨边界 new/delete；每个结构体以
  `struct_size_bytes` + `abi_version` 开头；句柄是不透明的 `uint64_t` 令牌；
  所有文本偏移显式命名为 `*_utf8_byte_offset` / `*_utf8_byte_count`。

新增语言身份不得靠追加 ABI 枚举值来"声明支持"。`EPOCANVA_BACKEND_ES/DE` 只是身份常量，
后端在缺少资源时拒绝打开。

### 2.3 Session / 编辑协议

**已实现（引擎侧）**，平台侧仍是"宿主保留原文 + 显式选择后替换"。

一个 Session 表示"一次从按下第一键到上屏或取消的输入过程"。已定义的状态与操作：

| 概念 | 当前实现 |
| --- | --- |
| 候选快照 | 带 `snapshot_token`、`context_target`、`generation` |
| 候选选择 | 需要快照令牌 + 候选令牌双重校验 |
| 提交 | `read_commit` 只复制；`ack_commit` 才推进上下文与学习 |
| 上下文 | 短期 ID 上下文，`set_context` 由调用方提供已确认文本 |
| 取消 | `cancel` 丢弃未提交状态 |
| 单位 | 所有偏移为 UTF-8 字节；`NEXT_UNIT` 的单位是**模型词法 token**，不是字素簇 |
| 外部修改/焦点切换 | 平台层以代际、目标范围、字段权限校验拒绝陈旧写入 |

**未实现**：完整中日韩行内 preedit、多段组合、部分确认、字素簇级编辑。

### 2.4 librime 边界

- `engine/adapter/rime_adapter.{h,cc}` 与 `engine/rime_components/*` 是唯一接触 librime 的地方。
- 现有形态是**编译进 `rime.dll` 的自有组件**（英文 Translator/Processor、探针），
  不是完整的封装层。ADR 0004 记录了该选择。
- 源码级改造走 `third_party/patches/`；该目录目前不存在，因为至今没有修改上游源码。
  这是**文档与现实的差距**，不是能力。

### 2.5 语言能力模块

已存在且可独立单测（`engine/languages/en`）：
词表与前缀查询、ASCII 大小写、词形与受保护文本、空格与提交边界、形态规则、
一次编辑拼写建议、连写分段。

`engine/languages/zh`：拼音音节/声母索引与有界简拼解码。
`engine/languages/fr`：法文核心与词典。
`engine/languages/ja`：Romaji/假名合成与 Mozc 数据驱动解码。
`engine/languages/ko`：两套式组字。

未实现：Unicode 大小写折叠、句首自动大写、专名大小写恢复、URL/邮箱/代码语法解析、
中文简繁与地区词表、法文完整形态生成、日文母语重写、韩文词尾与自动空格。

### 2.6 身份与排序层（本轮新增）

- `engine/locale`：BCP 47 子集解析与规范化、RFC 4647 lookup 匹配、默认文字、RTL 判定、
  七语言优先级、输入 profile 注册表与三级方案状态。
  这是**唯一**的语言身份来源；`engine/language_packages` 委托它。
- `engine/ranking`：有界确定性候选排序与学习事件语义。
  特征先钳制再加权；原样候选是硬分层；语言识别锁存；学习事件没有"按键/提交/退格"类型。
  它已被 `engine/backend/backend_api.cc` 在 **COMPLETION 候选**上只读使用：
  持久化偏好经 `LearningEventModel::SetPreference` 安装，后端既有顺序作为归一化先验，
  个性化项为有界的工程默认权重（未做语料校准）；原文候选不进入重排且保持首位。
  NEXT_WORD/NEXT_UNIT 在本阶段保持上下文模型顺序；扩展需阶段 E 的证据。

### 2.7 数据与学习存储

- `engine/data_store/learning_store.{h,cc}`：schema v3。保留 v2 的有界 word/pair 计数，
  新增 profile/language 头、候选稳定 ID、规范化词条身份、正/负摘要（千分点）、
  UTC 毫秒衰减时间戳、手动词条标志、半衰期与 revision。v1/v2 可迁移；格式损坏原子拒绝。
- 读取与写入分离：`Read`/`LoadSnapshot` 只要求 Normal 字段与当前 target/generation，
  **不要求** `learning_enabled`；`Record`/`RecordFeedback` 才要求录制开关。
  因此暂停录制停止新写入，但既有偏好仍参与排序；UI 明确说明这一点。
- `engine/data_store/windows_learning_repository.cc`：原子写（临时文件 + 刷盘 + 句柄重命名）、
  目录级共享锁、迁移前 `.v2` 备份、损坏文件 `.corrupt` 隔离、显式清除覆盖 data/tmp/backup/corrupt、
  `DeleteAll` 覆盖所有 profile。每个 profile 使用独立 catalog namespace。
- **产品持久学习仍关闭**：`ProductLearningWritesPermitted()` 为 false，因为 TSF 的字段权限
  判定是一次查询，不是覆盖排队、worker、确认和持久化的 OS 锁。产品会读取既有 v3 偏好，
  设置页显示"录制不可用"并提供真实清除入口；不能在无证据时宣称产品会写入学习。

### 2.8 可选神经层

- `engine/neural/lexical_reranker.{h,cc}` 有真实 llama.cpp 推理与 SHA-256 模型校验。
- 不接产品、默认不启用、无模型目录、无下载器。
- 架构约束：模型不得加载进宿主进程内的 TSF DLL；需要独立本地预测服务与严格 IPC，
  或 Android 侧的应用私有推理进程。

## 3. 依赖方向

见 §2 的图。补充规则：

- `engine/locale` 不依赖任何其他 engine 模块。
- `engine/language_packages` 依赖 `engine/locale`，不反向。
- `engine/ranking` 只依赖自身；不得包含平台头文件或 I/O。
- `engine/backend` 目前是 Windows-only（`CMakeLists.txt:3` 非 WIN32/MSVC 直接 fatal），
  这是 Android 复用的头号阻断项。

## 4. 与参考实现的关系

小狼毫（Weasel）与同文（Trime）作为 librime 平台集成的**参考资料**阅读，
用于核对既有行为；EpoCanva 不采用其外壳、不沿用其 ABI、不以其配置结构作为自身边界。
换皮发行违反 `docs/product/requirements.md` 第 1 节。

## 5. 当前实现状态

| 模块 | 状态 |
| --- | --- |
| toolchain probe | 已实现（P0.1） |
| CTest 基础设施 | 已实现；根工程当前 **78 条**用例（1 条符号链接用例在有权限限制时显式跳过） |
| librime 引入与构建 | 已锁定 librime 1.17.0 @ `33e7814…`，两套构建（baseline / component-probe） |
| 自有 C ABI | **已实现**：`epocanva_engine.h` v1 + `epocanva_backend.h` v1/v2，有 C 消费者测试 |
| engine backend session | **已实现**（单会话单语言；五语言资源绑定；确认/ACK 协议） |
| platform-win/tsf | **已实现注册导出与编辑路径**；本机未注册；当前包的真实宿主闭环未完成 |
| platform-win/tsf_product | 已实现控制器、候选窗口、权限门、STA 唤醒；产品只读既有偏好，持久写入因字段锁未证明而关闭 |
| platform-win/text | 已实现严格 UTF-8↔UTF-16 与边界表 |
| platform-android/ime | **未实现** |
| engine/locale | **已实现**（本轮）：BCP 47 + profile 注册表 + 3 条 CTest |
| engine/language_packages | 已实现；语言身份与优先级改为委托 `engine/locale` |
| engine/languages/en | 部分实现（见 §2.5）；不满足完整英文验收 |
| engine/languages/{zh,fr,ja,ko} | 部分实现，各有独立单测 |
| engine/lexical_service | 已实现（en/fr/ko 词法候选、拼写、形态、分段） |
| engine/prediction | 已实现（n-gram、缩写解码、前缀索引） |
| engine/ranking | 已实现；14 条 CTest；仅读偏好已接 COMPLETION 后端候选（有界、未校准）；NEXT_WORD 未接 |
| engine/data_store | schema v3（v1/v2 迁移、profile/language、衰减、正负反馈、手动词条、损坏隔离与清除）；产品持久写入仍关闭 |
| engine/neural | 推理已实现；未接产品；默认不启用 |
| 模型目录 / 下载 / 领域路由 | **未实现** |
| 安装包、签名、CI | **未实现** |
