# Windows 键盘桥接契约（P3.3：按键翻译与输入决策）

本文档描述 `platforms/windows/input/` 这一层**当前实际是什么**、决策如何产生、键盘状态与
`ToUnicodeEx` 如何被使用、哪些结果被拒绝、如何构建与验证，以及它**不是什么**。

本轮的范围是：**纯按键决策**（不含任何系统调用）、**Windows 键盘状态快照**、以及
**不改变键盘状态的文本翻译**。按键 sink、引擎桥接、宿主文本编辑、候选 UI、输入法注册都
不在本轮。本层没有任何代码能吞掉一个键、插入一个字符或修改一段组合区。

## 1. 这一轮是什么，不是什么

是：

- 一个纯函数 `DecideKey(event, text, context)`：输入是不可变的事件、翻译结果和上下文，
  输出是一个决策值。它不读全局键盘状态、不读文件、不调用引擎、不发消息、不修改输入状态，
  重复调用结果一致（`key_decision.{h,cc}`）。
- 一个 256 字节的 `GetKeyboardState` 快照，连带一个显式 HKL；修饰键在快照建立时推导一次，
  之后只读（`keyboard_state.{h,cc}`）。
- 一个 `ToUnicodeEx` 适配器，使用 `wFlags` bit 2（不改变键盘状态），只取返回值指定的
  UTF-16 码元，并把结果交给 P3.2 的严格转换（`key_text_translation.{h,cc}`）。
- 两个验证程序、5 条具名 CTest 用例：纯策略 2 条、真实 API 3 条。

不是：

- **不是 TSF 按键接入**。没有 `ITfKeyEventSink`、没有 `AdviseKeyEventSink`、没有
  `ITfKeystrokeMgr`，也没有任何 `pfEaten` 决策。`kEngine*Candidate` 只表示"如果存在引擎，
  这个事件可以交给它"，**不表示已经处理**。本层没有 `key_eaten` 字段。
- **不是实际应用输入**。没有宿主文本修改、没有 `ITfComposition`/`ITfTextInput`/`ITfRange`、
  没有 `SendInput`、没有 `keybd_event`、没有剪贴板访问。测试只翻译按键，不模拟键入。
- **不是 AltGr/dead key 的完整支持**。AltGr 以 Control+Alt 到达，本层明确按快捷键放行；
  dead key 只做"整体拒绝/边界协调需求"，不做重音组合输入。
- **不是引擎桥接**。没有 librime、没有 Session、没有 C ABI 会话结构；`engine/` 与
  `third_party/` 不参与本轮任何目标。

## 2. 目录与目标

```
platforms/windows/input/                 独立顶层 CMake 项目
  CMakeLists.txt                         项目、CRT、四个库目标
  key_event.h                            不可变事件/上下文/翻译结果词汇（无 <windows.h>）
  key_decision.{h,cc}                    纯决策函数与字符表
  keyboard_state.{h,cc}                  GetKeyboardState 快照、显式 HKL、修饰键推导
  key_text_translation.{h,cc}            不改变状态的 ToUnicodeEx 适配器
tests/windows/input_decision/            纯策略验证程序（2 条 CTest）
tests/windows/keyboard_translation/      真实 API 验证程序（3 条 CTest）
```

构建目标：

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `epocanva_key_decisions` | 纯决策，无系统调用 | 无（连 `<windows.h>` 都不包含） |
| `epocanva_input_text_encoding` | P3.2 的 `text_encoding.cc` **同一源文件** | kernel32 |
| `epocanva_keyboard_adapters` | 快照 + `ToUnicodeEx` | `epocanva_key_decisions`、编码库、user32 |
| `input_decision_tests` | 纯策略验证 | 只链纯决策库 |
| `keyboard_translation_tests` | 真实 API 验证 | 只链适配器库 |

`platforms/windows/input/CMakeLists.txt` 顶部有守卫：必须作为自己的顶层项目配置，理由与
P3.1/P3.2 相同——CRT 选择不能被根项目覆盖。本层用静态 `/MT`，与 `epocanva_tsf.dll` 一致。

**P3.2 复用的诚实边界**：本项目的 `epocanva_input_text_encoding` 直接编译
`platforms/windows/text/text_encoding.cc`，不是复制行为，也不是第二套编解码器。但 P3.2 的
独立项目仍是该文件的规范构建；**不能把两个项目各自的编码库同时链进同一个二进制**，否则
同一个符号会有两份定义。真正的统一定义留给将来把这几层合进一个 DLL 的那一轮。

## 3. 单位

AGENTS.md 第 5 节要求每个偏移、长度、索引都带单位。本层没有裸 `position` 或 `length`。

| 名称 | 单位 | 含义 |
| --- | --- | --- |
| `virtual_key` | Win32 虚拟键**码** | `VK_*` 值，不是字符 |
| `scan_code` | 硬件扫描**码** | 按下事件用 make code；bit 15 是抬起标记，本层不翻译抬起事件 |
| `key_state_bytes` | **字节**，固定 256 个 | 按虚拟键码索引；高位表示按下，低位表示 toggle |
| `repeat_count` | **次数**（宿主报告） | 元数据，不展开成动作 |
| `utf16_code_units` | **UTF-16 码元** | `ToUnicodeEx` 的返回单位；不是字素簇 |
| `utf8_bytes` | **UTF-8 码元（字节）** | P3.2 转换结果 |
| `ascii_code_point` | ASCII 范围内的 Unicode scalar value | 不是偏移、不是长度 |

`key_event.h` 不包含 `<windows.h>`，虚拟键常量从 SDK 抄写并测试钉住：
`winuser.h:479` `VK_BACK 0x08`、`:487` `VK_RETURN 0x0D`、`:509` `VK_ESCAPE 0x1B`、
`:516` `VK_SPACE 0x20`。

## 4. 决策层

### 4.1 disposition 与 reason

`KeyDisposition` 只有"交给宿主"、"快捷键放行"、"引擎候选"和"组合区边界需求"四类，
**没有"已处理/已吞掉"值**：

| disposition | 含义 |
| --- | --- |
| `kPassThroughToHost` | 宿主保留该键；本层不做任何事 |
| `kShortcutPassThrough` | Control/Alt/Windows 组合，归 OS 或宿主 |
| `kEngineTextCandidate` | 一个被接受的 ASCII scalar，可以交给引擎 |
| `kEngineSpaceCandidate` | 无修饰 Space，可以交给 P2.5 的 space policy |
| `kEngineEnterCandidate` | 组合中的无修饰 Enter |
| `kEngineBackSpaceCandidate` | 组合中的无修饰 BackSpace |
| `kEngineEscapeCandidate` | 组合中的无修饰 Escape |
| `kCompositionBoundaryRequired` | 有组合区，但本层无法决定内容放在哪里；需要后续轮次显式协调 |

`reason` 只说明**为什么**，不携带被拒绝的字符：`key-release`、`input-disabled`、
`permission-forbidden`、`permission-unknown`、`engine-not-ready`、`shortcut-modifier`、
`unsupported-modifier-combination`、`no-composition`、`backspace-without-composition`、
`no-text`、`dead-key`、`translation-failed`、`multiple-utf16-code-units`、
`non-ascii-character`、`unsupported-character`。

### 4.2 优先级（契约的一部分）

`DecideKey` 按以下顺序判断，测试逐条钉住：

1. key-up → pass-through / `key-release`
2. permission = forbidden → pass-through / `permission-forbidden`
3. permission = unknown → pass-through / `permission-unknown`
4. service disabled → pass-through / `input-disabled`
5. Control/Alt/Windows 任一按下 → `shortcut-pass-through` / `shortcut-modifier`
6. engine 未就绪 → pass-through / `engine-not-ready`
7. 四个特殊键的规则（下文）
8. 翻译结果规则（下文）

key-up 最先判断，因为它是事件结构事实；隐私判断先于快捷键、先于 enabled 标志；快捷键先于
engine 就绪。所有非引擎路径都是 pass-through，不存在"拒绝即吞掉"。

### 4.3 字符集合就是当前英文 schema 的 alphabet

字符表来自
`tests/fixtures/english_completion/epocanva_test_english.schema.yaml:108` 的
`speller/alphabet` 字面量：

```
abcdefghijklmnopqrstuvwxyz
ABCDEFGHIJKLMNOPQRSTUVWXYZ
0123456789
' @ . _ : / \ ? & = # % + -
```

共 **76** 个 ASCII 字符：52 个字母、10 个数字、2 个 token delimiter（`'`、`-`）、
12 个 protected mark（`@ . _ : / \ ? & = # % +`）。分类计数由纯测试从 0..127 枚举得到，
不是从注释里抄的。

- 字母 → `kAsciiLetter`；数字 → `kAsciiDigit`；`'`/`-` → `kTokenDelimiter`；
  其余 12 个 → `kProtectedMark`。
- **数字和技术标记不被丢弃**。它们进入引擎候选，正是为了让 P2.3 的整 token 保护规则能在
  组合区里看到它们，从而拒绝把邮箱、凭据、版本号的前半段拿去补全。本层不把"能产生补全"
  当作接收条件。
- 其他 ASCII（空格、逗号、分号、括号、控制字符等）→ `unsupported-character`；
  非 ASCII → `non-ascii-character`；多个 UTF-16 码元 → `multiple-utf16-code-units`。
  这些结果**整体拒绝**，不会只取第一个码元。

**schema 注释的一处自相矛盾**（不在本轮修改）：同一 YAML 的注释写 "the eleven technical
marks"，但紧跟其后的列表有 12 个（`@ . _ : / \ ? & = # % +`），第 108 行的 alphabet
也确实包含 12 个。本层以**字面量**为准，测试钉住 76/52/10/2/12；注释里的 "eleven" 是
documentation drift，不是本层的字符表。

### 4.4 特殊键

| 键 | 条件 | 结果 |
| --- | --- | --- |
| Space | 无修饰，service 开、engine 就绪、permission allowed | `kEngineSpaceCandidate`，无论是否有组合 |
| Enter | 无修饰 + 有组合 | `kEngineEnterCandidate` |
| Enter | 无修饰 + 无组合 | pass-through / `no-composition`（保留宿主换行语义） |
| Escape | 无修饰 + 有组合 | `kEngineEscapeCandidate` |
| Escape | 无修饰 + 无组合 | pass-through / `no-composition` |
| BackSpace | 无修饰 + 有组合 | `kEngineBackSpaceCandidate` |
| BackSpace | 无修饰 + 无组合 | pass-through / `backspace-without-composition` |
| 以上四键 + Shift | 任意 | pass-through / `unsupported-modifier-combination` |

Shift+Space、Shift+Enter、Shift+BackSpace、Shift+Escape **没有产品规则**，本层不发明规则，
只明确标记为不处理。Caps Lock 不是修饰键，不阻止 Space 或文本候选；它只影响翻译结果。

BackSpace 在无组合时不发给引擎，是任务书明确要求的宿主编辑语义；Enter/Escape 在无组合时
同理放行，因为本层没有提交/换行规则可以声称正确。

### 4.5 快捷键与 AltGr

Control、Alt、Windows 任一按住 → `kShortcutPassThrough`。这包括：

- Ctrl+C、Ctrl+V、Ctrl+Space；
- Alt+字母、Win+字母；
- **Control+Alt 形状的 AltGr**。

AltGr 在本轮**不被当作已支持的文本输入**。它没有单独的字段，也不会被拆出来尝试翻译；
它就是 Control+Alt，走快捷键放行。测试钉住 Ctrl+C、Ctrl+V、Ctrl+Space、Alt+A、Win+A、
Ctrl+Alt+A 六个组合。

### 4.6 隐私与可用性

`InputPermission` 有 `allowed` / `forbidden` / `unknown` 三值。`unknown` 按保守处理：
**不产生引擎候选**。理由不是"可能泄露"，而是"转发就是启动组合/学习的第一步"；分类不明时
不迈出这一步。forbidden 与 unknown 都 pass-through，但 reason 分开，避免把"隐私拒绝"
与"分类失败"混为一谈。

`input_enabled=false` 与 `engine_ready=false` 同样 pass-through，reason 分开。默认构造的
`InputContext` 是 disabled + engine not ready + permission unknown，只能得到 pass-through。

本层**没有**字段级分类器：没有 `TATTE_*` 查询、没有焦点字段名、没有密码字段探测。它只
消费调用方给出的 permission 值；真正的分类属于后续 TSF 适配轮次。

### 4.7 组合区边界

有组合区时：

- 一个被接受的 ASCII 字符 → `kEngineTextCandidate`（仍只是候选）；
- 一个 dead key → `kCompositionBoundaryRequired` / `dead-key`；
- 多码元文本 → 边界需求 / `multiple-utf16-code-units`；
- 非 ASCII → 边界需求 / `non-ascii-character`；
- 其他 ASCII → 边界需求 / `unsupported-character`；
- 无文本（翻译返回 0）→ pass-through / `no-text`，不声称边界已解决；
- 翻译失败 → pass-through / `translation-failed`。

边界的语义是**明确的未解决**：本层不声称"直接放行就对了"，也不声称标点顺序、提交顺序或
组合区归属已经处理。它只把"这里必须由后续轮次协调"变成一个可测试的返回值。无组合区时，
同样的不支持文本只是 pass-through / `unsupported-character`。

### 4.8 重复、抬起与幂等

- 一次回调 → 一个事件 → **一个决策**。`repeat_count` 只作为元数据携带，绝不展开成多次提交。
  测试用 `repeat_count=1` 与 `=5`、`previously_down` false/true 比较，决策完全相同。
- key-up → `kPassThroughToHost` / `key-release`，不进入英文文本路径，不翻译，不产生边界需求。
- `DecideKey` 是纯函数：同一组输入连续评估三次，决策完全相同；事件、翻译结果、上下文
  一个字段都不变。测试显式比较三次结果和输入字段。

## 5. Windows 键盘状态快照

### 5.1 捕获

`CaptureThreadKeyboardState()` 只做两件事：

1. `GetKeyboardState(PBYTE)` 填 **256 字节**（`winuser.h:5934-5939`）；
2. `GetKeyboardLayout(0)` 取调用线程的当前 HKL，在适配边界**显式**记录。

`GetKeyboardState` 文档的语义（Microsoft Learn, GetKeyboardState）：高位为 1 表示按下；
低位的 toggle 状态只对 Caps Lock 有意义。推导规则：

| 字段 | 来源 |
| --- | --- |
| `shift` | `key_state_bytes[VK_SHIFT]` 高位 |
| `control` | `key_state_bytes[VK_CONTROL]` 高位 |
| `alt` | `key_state_bytes[VK_MENU]` 高位 |
| `windows` | `VK_LWIN` 或 `VK_RWIN` 高位 |
| `caps_lock` | `key_state_bytes[VK_CAPITAL]` **低位**（toggle，不是按住） |

`VK_SHIFT`/`VK_CONTROL`/`VK_MENU` 作为数组索引同时覆盖左右实例，这是文档明确允许的；
Windows 键有两个，所以两个都检查。常量行号：`winuser.h:493-497`、`:539-540`。

### 5.2 不可变

`KeyboardStateSnapshot` 只有 const 访问器，没有 setter。修饰键在
`FromExplicitState` 里推导一次，之后不再读全局状态。字节数组按值复制；测试在构造后修改
源数组，快照的字节和修饰键都不变。测试还用两个受控数组构造两个快照，证明它们各自读取
自己的字节，不会回落到机器的真实键盘。

### 5.3 禁止项

`keyboard_state.cc` **没有**调用 `SetKeyboardState`、`GetAsyncKeyState`、
`AttachThreadInput`、`ActivateKeyboardLayout`、`LoadKeyboardLayout` 或注册表 API。
它不设置全局键盘状态，不安装/激活布局，不读文件。256 字节是状态，不是文本；本文件没有
任何日志会打印这些字节。

## 6. ToUnicodeEx 文本翻译

### 6.1 调用的确切形状

```c
ToUnicodeEx(virtual_key, scan_code,
            snapshot.key_state_bytes(),        /* 256 bytes */
            buffer, kTranslationBufferUtf16CodeUnits,
            kToUnicodeNoKeyboardStateChangeFlag /* 0x0004, bit 2 */,
            snapshot.layout());
```

依据：

- 声明：安装的 SDK `winuser.h:1273-1285`；`lpKeyState` 的 SAL 是
  `_In_reads_bytes_(256)`（第 1280 行），`dwhkl` 是 `_In_opt_ HKL`（第 1284 行）。
- `wFlags` bit 2："keyboard state is not changed (Windows 10, version 1607 and newer)"
  （Microsoft Learn, ToUnicodeEx）。**最低支持条件是 Windows 10 1607**；本层不声称兼容更旧
  的 Windows，也没有"关闭 bit 2 再自己恢复状态"的回退实现。
- bit 0（菜单激活）与 bit 1（翻译 break 事件）**不设置**；其余位保留，也不设置。
- `dwhkl` 始终来自快照，不在翻译时重新取当前布局，避免事件与翻译之间切换布局导致字符变化。

### 6.2 返回值处理

| `ToUnicodeEx` 返回 | 适配器行为 | 决策层结果 |
| --- | --- | --- |
| `< 0` | `kDeadKey`，清空缓冲，**不使用** API 可能写入的 spacing character | 有组合 → 边界需求 / `dead-key`；无组合 → pass-through / `dead-key` |
| `= 0` | `kNoText`，清空缓冲 | pass-through / `no-text` |
| `> 0` | 只复制返回值指定数量的 UTF-16 码元 | 交给决策层逐条分类 |
| `>= 32`（缓冲容量） | `kSystemFailure`，拒绝 | pass-through / `translation-failed` |

缓冲**不假设 NUL 终止**：长度来自返回值，不来自 `wcslen`。返回值之后可能还有无效字符，
一个也不复制。负返回值不重试、不循环、不"清空 dead key"。

### 6.3 严格转换与整体拒绝

`>0` 的码元交给 P3.2 的 `ConvertUtf16CodeUnitsToUtf8Bytes`（同一源文件）：

- 转换失败（孤立代理项等）→ `kInvalidText`，清空 UTF-16 与 UTF-8 结果，保留 P3.2 的
  无文本诊断和 `os_error`；决策层得到 `translation-failed`；
- 转换成功 → `kText` + 精确 UTF-16 码元 + UTF-8 字节。

适配器**不**自行判断"非 ASCII 就丢弃"或"多字符取第一个"：它保留完整结果，由决策层在
字符表层面整体拒绝。这样非 ASCII/多码元在有组合时能升级为明确的边界需求，而不是被适配器
悄悄截断。测试对多码元、代理对、非 ASCII 都断言 `ascii_code_point == 0`、`text_class` 为空。

### 6.4 副作用禁令

本层没有 `SendInput`、`keybd_event`、`SetKeyboardState`、布局切换、剪贴板、引擎调用。
key-up 在调用 API **之前**就被拒绝（`kNotKeyDown`，`system_call_made == false`），
所以抬起事件不可能触碰 dead-key 状态。测试还钉住：没有 HKL 时也在调用前拒绝
（`kNoLayout`）。

### 6.5 非突变性质的证据边界

bit 2 的"不改变键盘状态"在**有 dead key 的布局上**可以做直接行为测试：先翻译 dead key，
再用同一布局翻译 `a`；如果 bit 2 被忽略，`a` 会被组合成 `á` 之类的字符。本轮测试实现了
这个探针，但本机 4 个已加载布局的 neutral/Shift 扫描**没有发现 dead key**，所以该探针
在本机**未验证**（见 §7.4）。常量 `0x0004`、SDK/MSDN 契约和代码审查是当前的证据；不能把
"纯测试里的 dead-key 拒绝"说成"真实布局的 dead-key 非突变已验证"。

## 7. 测试

### 7.1 纯策略与真实 API 分开

两个可执行文件、两组 CTest：

- `input_decision_tests` 只链 `epocanva_key_decisions`，不含 `<windows.h>`，不调用任何
  Win32 API。它证明决策表、优先级、字符表、幂等性。
- `keyboard_translation_tests` 只链适配器，调用真实 `GetKeyboardState` /
  `GetKeyboardLayout` / `ToUnicodeEx`。它证明快照与翻译在**本机已加载布局**上的行为。

一个纯测试通过**不能**被报告成"真实布局已支持"；一个真实 API 测试通过也**不能**被报告成
"纯规则表所有分支都已覆盖"。

### 7.2 纯策略结果

直接运行 `--mode decisions`：**91 checks，0 failed**，退出 0。覆盖：

- a/A；76 个 schema 字符逐个成为候选，52/10/2/12 分类计数；
- 字符表对其余 52 个 ASCII 逐个返回"不支持"；决策层再用逗号、由非 Space 键产生的空格、
  控制字符抽样验证拒绝路径；
- Space/Enter/BackSpace/Escape 的有/无组合语义；Shift+四键全部不处理；
- Ctrl+C、Ctrl+V、Ctrl+Space、Alt+A、Win+A、Ctrl+Alt（AltGr）；
- key-up；repeat_count 1 vs 5、previous_down false/true；
- 无文本、dead key（含"缓冲里有 spacing char 也不使用"）、多码元、代理对、非 ASCII、
  不支持 ASCII、空 kText、四种翻译失败状态；
- forbidden/unknown permission、input disabled、engine not ready，以及它们的优先级；
- 有组合时的不支持文本/dead key/多码元/非 ASCII → 边界需求；
- 同一输入三次评估结果相同、输入字段未变。

日志：`build/windows-input-stage1-decisions-direct-r2.log`（stage 1 构建）与
`build/windows-input-decisions-direct-r1.log`（最终构建），两者都是 91/0。

### 7.3 真实 API 结果

最终构建直接运行：

| 模式 | 检查数 | 结果 | 日志 |
| --- | --- | --- | --- |
| `--mode snapshot` | 15 | 0 failed，退出 0 | `build/windows-input-snapshot-direct-r1.log` |
| `--mode translation` | 331 | 0 failed，退出 0 | `build/windows-input-translation-direct-r1.log` |
| `--mode self-check-fail` | 1 | 1 failed，退出 **14** | `build/windows-input-translation-can-fail-direct-r1.log` |

**实际测试布局**（`GetKeyboardLayoutList`，只读枚举，不安装、不激活）：

| HKL 低字（language id） | 标签 | 76 个 schema 字符 |
| --- | --- | --- |
| `0x0404` | 中文（繁体）布局 | 76/76 逐字符精确翻译 |
| `0x0804` | 中文（简体），**当前线程布局**，KLID `00000804` | 76/76 |
| `0x0409` | 英语（美国） | 76/76 |
| `0x0411` | 日语 | 76/76 |

四个布局都能直接产生这 76 个 ASCII 字符（language id 不同，HKL 低字不同）。测试没有把它们
当成 US 布局：每个字符的测试方式是**布局相对**的——`VkKeyScanExW(ch, hkl)` 由布局自己给出
虚拟键与 Shift 状态，再用该状态构造受控 256 字节数组，`ToUnicodeEx` 必须返回恰好该字符。
没有把 US 的 VK→字符结果硬套到其他布局；例如在 AZERTY 上同一物理键会得到布局自己声明的
字符。

其他实测项：

- 每个布局上的 `a` → U+0061、`A` → U+0041，且翻译结果进入纯决策层得到
  `kEngineTextCandidate`；
- 函数键 `VK_F1` 在扫描中返回 0 → `kNoText`，`raw=0`、0 码元（至少一个布局实测，
  证据行 `evidence no-text ... vk=0x70 raw=0`）；
- key-up → `kNotKeyDown`，`system_call_made=no`，不调用 API；
- 无 HKL → `kNoLayout`，不调用 API；
- `repeat_count=5` + `previously_down=true` → 仍是一个结果、一个字符；
- 快照：真实捕获成功、256 字节、当前 HKL；受控数组推导 Shift/Ctrl/Alt/左/右 Win/Caps Lock；
  低位/高位区别；bit 6 忽略；构造后修改源数组不影响快照；两个快照互不影响。

### 7.4 本机未验证的范围（明确列出）

以下分支在**纯测试**里有完整覆盖，但**真实 API 在本机未验证**，因为 4 个已加载布局都是
US 键位拉丁布局：

| 场景 | 本机状态 | 原因 |
| --- | --- | --- |
| dead key 真实返回 `< 0` | **未验证** | neutral/Shift 键扫描未发现 dead key |
| bit 2 的 dead-key 非突变行为探针 | **未验证** | 同上；没有 dead key 可先按下 |
| 非 ASCII 单码元真实翻译（é/ü/ñ/ф/あ/中/Ω/€） | **未验证** | 没有布局提供非 Ctrl/Alt 的直接 chord |
| 多码元真实翻译 | **未验证** | 扫描未发现 `raw > 1` |
| AltGr 产生字符 | **未验证，且不支持** | 本层按 Control+Alt 快捷键放行 |

这些不是"实现失败"，也不是"已验证通过"。报告与本文档都不把它们写成绿。纯测试证明的是：
**如果**适配器交来 dead key/多码元/非 ASCII，决策层会整体拒绝并按组合状态给出正确的
pass-through 或边界需求；它不证明真实布局会产生这些结果，也不证明 bit 2 在 dead-key
场景下的非突变——后者需要一台装有 dead-key 布局的机器（或后续轮次经授权的布局测试环境）。

### 7.5 可失败性控制

- `windows.input.decisions_can_fail`：故意断言"dead key 会变成文本候选"，必须退出 **14**
  且日志不含 `RESULT PASS`。
- `windows.input.translation_can_fail`：故意断言"key-up 会被翻译成文本"，必须退出 **14**
  且日志不含 `RESULT PASS`。

没有这两条，前面所有绿色都可能只是同义反复。

### 7.6 CTest

过滤匹配先验证非空：

```
Total Tests: 5
  windows.input.decisions
  windows.input.decisions_can_fail
  windows.input.keyboard_snapshot
  windows.input.text_translation
  windows.input.translation_can_fail
```

`ctest -R "^windows\.input\." --no-tests=error --output-on-failure`：**5/5 Passed，退出 0**
（`build/windows-input-ctest-r1.log`）。日志没有被覆盖：配置、构建、直跑、CTest 各用新文件名。

### 7.7 构建过程与真实失败

| 步骤 | 退出码 | 说明 / 日志 |
| --- | --- | --- |
| stage 1 配置 | 0 | `build/windows-input-stage1-configure-r1.log` |
| stage 1 构建 r1 | 0 | `build/windows-input-stage1-build-r1.log` |
| stage 1 直跑 decisions r1 | **5** | 3 条失败：测试辅助函数把 Enter/BackSpace/Escape 也构造成了 VK_A。属于测试自身的错误，不是实现错误；见 `build/windows-input-stage1-decisions-direct-r1.log` |
| 修正测试辅助函数 → stage 1 构建 r2 | 0 | `build/windows-input-stage1-build-r2.log` |
| stage 1 直跑 decisions r2 | 0，91/0 | `build/windows-input-stage1-decisions-direct-r2.log` |
| stage 1 CTest | 0，2/2 | `build/windows-input-stage1-ctest-r1.log` |
| **提交 1** | — | `d32509448bca509573147f8f120ea8757146c01c` |
| stage 2 配置 | 0 | `build/windows-input-configure-r1.log` |
| stage 2 构建 r1 | **1** | `error C2665`：`std::u16string::assign` 没有接受 `const wchar_t*` 的重载。修正为逐码元 `static_cast<char16_t>` 复制；`build/windows-input-build-r1.log` |
| stage 2 构建 r2 | 0，0 warning | `build/windows-input-build-r2.log` |
| stage 2 直跑（snapshot / translation / can-fail） | 0 / 0 / **14** | 见 §7.3 的日志 |
| stage 2 CTest | 0，5/5 | `build/windows-input-ctest-r1.log` |

构建命令（工具链与 P3.1/P3.2 相同：VS 2022 Professional 自带的 CMake/CTest 3.31.6-msvc6，
生成器 `Visual Studio 17 2022`，`-A x64`，Release）：

```sh
CMAKE="<VS Pro>/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
CTEST="<同上目录>/ctest.exe"

"$CMAKE" -S platforms/windows/input -B build/windows-input/test \
      -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
"$CMAKE" --build build/windows-input/test --config Release

"$CTEST" --test-dir build/windows-input/test -C Release -N -R "^windows\.input\."
"$CTEST" --test-dir build/windows-input/test -C Release -R "^windows\.input\." \
      --no-tests=error --output-on-failure
```

本机绝对路径记在 git-ignored 的 `docs/reports/environment.local.md`；本文档不复制。

## 8. 回归

全部在本轮构建成功之后运行：

| 回归项 | 命令要点 | 退出码 | 结果 | 日志 |
| --- | --- | --- | --- | --- |
| P3.1 TSF 生命周期 | `ctest --test-dir build/windows-tsf/test -C Release -R "^tsf\.lifecycle\." --no-tests=error` | 0 | 3/3 Passed（过滤前 `-N` 报 `Total Tests: 3`） | `build/windows-input-tsf-regress-r1.log` |
| P3.2 文本层 | `ctest --test-dir build/windows-text/test -C Release -R "^windows\.text\." --no-tests=error` | 0 | 4/4 Passed（过滤前 `-N` 报 `Total Tests: 4`） | `build/windows-input-text-regress-r1.log` |
| 英文正式入口 | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 -EnginePrefix build/librime-english/rime/install -BaselinePrefix build/librime-baseline/rime/install -BuildDir build/english-recovery/test -LogTag p33regress` | 0 | `RESULT PASS 29 case(s) selected, 0 failed, ctest exit 0` | `build/windows-input-english-regress-console-r1.log` |

`-LogTag p33regress` 是新的，英文入口自己拒绝覆盖已存在的同名日志。

## 9. 明确未实现 / 不得作出的声明

以下都不是"藏在别处"，是不存在：

- `ITfKeyEventSink`、`AdviseKeyEventSink`、`ITfKeystrokeMgr`、`pfEaten`；
- 引擎 Session、librime、C ABI 会话、组合区提交；
- `ITfComposition`/`ITfTextInput`/`ITfRange`、宿主文本插入/删除/回退；
- 候选 UI、输入法注册/安装/profile 登记；
- AltGr 文本输入、dead-key 重音组合、非 ASCII 文本输入；
- 字素簇光标/删除、文本身份/版本校验；
- 字段级隐私分类（`TATTE_*`）、密码字段探测；
- 网络 I/O（本层只链 user32/kernel32，无 socket/WinHTTP/WinINet 调用）。

因此本轮结束时，即使全部测试为绿，也**仍然不是一个能装上就用的输入法**，更不是
"TSF 按键接入完成"或"实际应用输入完成"。本层只回答：一个按键事件在纯规则下应当被
放到哪个路径；Windows 快照与翻译是否按文档契约工作；不支持的结果是否被诚实拒绝。

## 10. 依赖边界与下一阶段

新增的直接依赖只有 user32 的 `GetKeyboardState`、`GetKeyboardLayout`、`ToUnicodeEx`
（以及 kernel32 的 `GetLastError`）。没有 ole32/uuid、没有新增第三方库、没有改根项目
`CMakeLists.txt`/`CMakePresets.json`、没有触碰 `engine/` 与 `third_party/`。

下一阶段（不属于本轮）需要：

1. 在 TSF 的 `ITfKeyEventSink` 中把 `wParam`/`lParam` 解码成 `KeyEventSnapshot`：
   repeat count、previous state、extended、scan code，并调用本层的 `DecideKey`；
2. 根据 `DecideKey` 的结果决定 `pfEaten` 与引擎投递——这一步才会产生真正的"已处理"；
   必须单独验证，不能从本层的 `kEngine*Candidate` 推断；
3. 引擎桥接与组合区：C ABI 会话、UTF-16↔UTF-8 边界换算、preedit/commit；
4. 组合区存在时的标点/提交顺序协调，以及 §4.7 的边界需求落地；
5. AltGr/dead-key 的完整产品规则（需要明确的产品决策，不是本层能顺手定义的）；
6. 字段级隐私分类与 TSF 焦点字段读取；
7. 注册与安装流程（需要单独授权与验证）。

这些都不在本轮代码里，也不应从本轮的绿色测试中推断出来。

## 11. 本轮提交

- 提交 1（纯决策与纯测试）：`d32509448bca509573147f8f120ea8757146c01c`
  `feat(windows): add deterministic keyboard input decisions`
- 提交 2（适配器、真实 API 测试与本文件）：即包含本文件的那次提交
  `feat(windows): add non-mutating keyboard text translation`。Git 提交无法包含自己的
  SHA，完整 SHA 见本轮阶段报告。

两次提交之间各自完成了配置/构建/测试；没有先把终态写完再倒推 stage 1。
