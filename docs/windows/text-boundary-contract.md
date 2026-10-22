# Windows 文本编码与位置边界契约（P3.2）

本文档描述 `platforms/windows/text/` 这一层**当前实际是什么**、单位如何定义、非法输入如何
处理、如何构建与验证，以及它**不是什么**。P3.2 的边界是编码换算与位置边界映射；键盘、组合区、
候选 UI 与注册都不在本轮范围内。

## 1. 这一轮是什么，不是什么

是：

- 一套**严格** UTF-8 ↔ UTF-16 转换（`text_encoding.{h,cc}`）：只走系统 API，非法即失败，
  不替换、不清洗、不规范化、不改大小写；
- 一张与**同一份不可变 UTF-8 文本**绑定的边界表（`text_boundary_map.{h,cc}`），
  在 UTF-8 字节偏移与 UTF-16 代码单元偏移之间做双向换算，越界与序列内部偏移一律拒绝；
- 一个验证程序 `text_bridge` 与 4 条具名 CTest 用例。

不是：

- **不是输入法的一部分**。没有键盘 sink（`ITfKeyEventSink`）、没有宿主组合区
  （`ITfComposition`/`ITfTextInput`）、没有候选 UI、没有 `ToUnicodeEx` 翻译、没有任何注册或
  安装动作。本轮连 DLL 都不产出。
- **不是通用 Unicode 编辑层**。没有 NFC/NFD 规范化、没有字素簇光标或删除、没有大小写转换、
  没有文本缓冲管理、没有异步编辑协议。"通用 Unicode 编辑已完成"这句话**不成立**，本文档
  与测试输出都不做这种暗示。
- **不是引擎桥接**。没有 librime、没有 Session、没有 C ABI 会话结构；`engine/`、
  `third_party/` 与本轮任何目标无关。

## 2. 目录与目标

```
platforms/windows/text/         独立顶层 CMake 项目（不是根项目的子目录）
  CMakeLists.txt                项目、CRT、两个静态库目标
  text_encoding.{h,cc}          严格 UTF-8 <-> UTF-16 转换
  text_boundary_map.{h,cc}      UTF-8 字节偏移 <-> UTF-16 代码单元偏移边界表
tests/windows/text_bridge/      验证程序与 4 条具名 CTest 用例
  CMakeLists.txt                被上面的项目 add_subdirectory 进来
  main.cc                       encoding / boundary / guards / self-check-fail 四种模式
```

`platforms/windows/text/CMakeLists.txt` 顶部有守卫：它**必须**作为自己的顶层项目配置，理由与
P3.1 相同——CRT 选择不能被根项目覆盖。这一层将来要被链进 `epocanva_tsf.dll`，而根项目是
Debug / 动态 CRT；两边都用 `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`（静态 `/MT`），
所以未来链接不会把一个进程里塞进两套 CRT。

`tests/windows/text_bridge/CMakeLists.txt` 也有守卫：单独配置时它没有可判断的库。

与 P3.1 的一个真实差别：验证程序**直接链接**这两个静态库，而不是像 `tsf_lifecycle` 那样用
`LoadLibrary` 取 DLL。`tsf_lifecycle` 必须那样做，因为未注册的 COM 服务器只有经装载器才能碰到；
本轮的被测对象是纯函数，直接链接才是诚实的耦合，也不存在搜索顺序撞车的风险。

## 3. 单位

AGENTS.md 第 5 节要求每个偏移都带单位，本节是这份契约的落点。

| 名称 | 单位 | 含义 |
| --- | --- | --- |
| `utf8_bytes` / `utf8_byte_length` / `utf8_byte_offset` | **UTF-8 码元（字节）** | 从文本起始字节的字节数 |
| `utf16_code_units` / `utf16_code_unit_length` / `utf16_code_unit_offset` | **UTF-16 码元** | 从文本起始码元的码元数 |
| `boundary_count()` | 个数 | 合法边界数量 =  scalar 值数量 + 1（终点偏移是其中一个） |

本层**没有**任何名为 cursor / position / length 而不带单位的量。没有"屏幕列"单位，也没有
"字素簇"单位——两者都不在本轮实现范围内，未来 preedit 定位必须自己定义并说明列的口径。

`std::wstring` / `std::wstring_view` 在本层就是 **Windows UTF-16 序列**。头文件里有一条
`static_assert(sizeof(wchar_t) == 2, ...)`：这个类型名在这个平台上不代表可移植宽字符，
在 `wchar_t` 为 32 位的平台上这份契约直接编译失败，而不是把每个偏移静默解释错。

## 4. 编码转换契约

### 4.1 使用的系统 API

| 方向 | API | CodePage | flags |
| --- | --- | --- | --- |
| UTF-8 → UTF-16 | `MultiByteToWideChar` | `CP_UTF8` | `MB_ERR_INVALID_CHARS` |
| UTF-16 → UTF-8 | `WideCharToMultiByte` | `CP_UTF8` | `WC_ERR_INVALID_CHARS` |

取证依据（本机已安装的 SDK，Windows 10.0.26100.0）：

- `um/stringapiset.h:118-130` —— `MultiByteToWideChar` 声明；长度参数是 `int cbMultiByte`
  （第 127 行）、`int cchWideChar`（第 129 行），且输出参数标 `_Out_writes_to_opt_`，
  即"NULL 指针 + 0 容量"的查询模式是被声明本身允许的（SAL 的 `_opt`）。
- `um/stringapiset.h:132-146` —— `WideCharToMultiByte` 声明；`int cchWideChar`（第 141 行）、
  `int cbMultiByte`（第 143 行），输出 `_Out_writes_bytes_to_opt_`。两个默认字符参数
  （`lpDefaultChar` / `lpUsedDefaultChar`）在本层**恒为 NULL**：CP_UTF8 下 SDK 不提供
  默认字符回退，正好与"禁止静默替换"一致。
- `um/WinNls.h:148` —— `MB_ERR_INVALID_CHARS 0x00000008`。
- `um/WinNls.h:158-160` —— `WC_ERR_INVALID_CHARS 0x00000080`，`#if (WINVER >= 0x0600)`。
  这一层能编译通过本身就是该门槛已满足的证据。
- `um/WinNls.h:143-147` 的注释明确写着 `MB_PRECOMPOSED` / `MB_COMPOSITE` 已废弃并指向
  `NormalizeString`。本层不使用它们中的任何一个，也**不调用** `NormalizeString`：
  规范化不在本轮范围内（§7）。

### 4.2 十条要求与它们的落点

| # | 契约 | 落点 |
| --- | --- | --- |
| 1 | 输入长度显式传递 | 参数是 `std::string_view` / `std::wstring_view`，长度取自视图；`-1`（NUL 结尾约定）在代码里不出现 |
| 2 | 空输入明确成功 | `text_encoding.cc` 在调用任何 API **之前**短路返回 `kOk` + 空输出；测试断言 `os_error == 0`，即"确实没有失败的系统调用" |
| 3 | 保留嵌入 NUL | 长度来自视图而非 `strlen`；测试用 `41 00 42` 证明 3 字节 ↔ 3 码元，反向同样回到 3 字节且第 2 个字节仍是 `00` |
| 4 | 单位写清 | 每个函数名、字段名、参数名都带 `bytes` 或 `code_units`（§3） |
| 5 | 调用前检查 `size_t` → `int` | `CheckUtf8InputByteLength` / `CheckUtf16InputCodeUnitLength` / `CheckUtf8OutputCapacityForUtf16CodeUnits`，纯算术，先于任何系统调用与任何输入读取 |
| 6 | 先查询容量，再转换，核对写入长度 | 每个方向两次调用：`nullptr,0` 查询 → 按查询值分配 → 实际转换 → 比较 `written != required` 即 `kLengthMismatch`；最终输出长度以**实际写入数**为准 |
| 7 | 系统失败保留可诊断信息 | 结果带 `os_error`（`GetLastError()`）与 `diagnostic`（API 名 + 阶段 + gle） |
| 8 | 失败与合法空输出可区分 | `status` 字段；测试用"空输入 → kOk 且输出空"与"截断序列 → kInvalidInput 且输出空"两条并列断言二者状态不同 |
| 9 | 失败不暴露半成品 | 每条失败路径都 `buffer.clear()` 后再返回；边界构建失败时返回的表是**空文本的表**，不是部分构建的表 |
| 10 | 不记录传入文本 | `diagnostic` 只由两个字面量与一个数字构成；测试用 `61 … "secret" + C3` 断言诊断串里既没有 `secret` 也没有 `eci`，同时仍含 `MultiByteToWideChar` 与 `gle=` |

### 4.3 禁止项的落实位置

`CP_ACP` 不作为代码出现：在 `platforms/windows/text/` 与 `tests/windows/text_bridge/` 里
`grep -rn CP_ACP` 命中 **1 处，且就是下面这份禁止清单自身的注释**，作为 API 实参命中 **0 处**
（`grep -cE "MultiByteToWideChar\([^)]*CP_ACP|WideCharToMultiByte\([^)]*CP_ACP"` → 0）。
默认字符/最佳匹配字符参数为 NULL；输入原样交给系统，不做任何预清洗；没有手写编解码器；
`NormalizeString` / `CharUpper` / `CharLower` / `LCMapString` / `towupper` / `towlower`
在本层的调用点为 0；失败不返回成功；不打印文本。这些不是注释里的愿望，§4.2 表里每一行都有
对应断言。

`guards` 模式还钉住一条更難自证的：**越界长度必须在读取输入之前被拒绝**。测试构造
`std::string_view(nullptr, INT_MAX+1)` 与 `(nullptr, SIZE_MAX)` 交给两个转换入口和边界构建器。
只读 `.size()` / `.empty()` 是安全的；如果实现漏了长度闸门就去调 API，那会是对 NULL 的解引用，
测试当场崩溃而不是"绿"。断言里额外要求 `os_error == 0`——真正的"没有发生系统调用"证据。
**不分配数 GB 内存，也不伪造有效缓冲区再越界读**。

## 5. 边界映射契约

### 5.1 表的内容

`BuildUtf8BoundaryMap(不可变 UTF-8 文本)` 产出一张升序表，每行同时给出两个单位：

```
struct Utf8Utf16Boundary { std::uint32_t utf8_byte_offset; std::uint32_t utf16_code_unit_offset; }
```

`uint32_t` 是无损的：入口闸门已把字节长度限制在 `INT_MAX` 以内，而一个良构 UTF-8 → UTF-16
转换**不会膨胀**（1 字节 → 1 码元，2/3 字节 → 1 码元，4 字节 → 2 码元），所以两个偏移都小于
`INT_MAX`。

构建过程三步，全部可核对：

1. 用 §4 的严格转换判定良构性，并拿到系统给出的 UTF-16 码元总数；
2. 沿文本走一遍**序列起始字节**，每步只问两件事：这段占几个字节、变几个码元
   （`ShapeOfLeadingByte`：`0x00-0x7F`→1/1，`0xC2-0xDF`→2/1，`0xE0-0xEF`→3/1，
   `0xF0-0xF4`→4/2）。这不是第二套编解码器：不解码 scalar 值、不看延续字节、
   不做任何合法性判断，只在系统已接受的文本上取长度；
3. 走出来的码元总数必须与系统转换给出的**相等**，否则整张表丢弃、返回 `kLengthMismatch`。
   这条交叉检查是两个独立算法对同一文本的一致性判断——它可能失败，所以它有价值。

### 5.2 拒绝，而不是取整

| 输入 | 返回 | 出参 |
| --- | --- | --- |
| 表内偏移（含终点偏移） | `kMapped` | 写入映射结果 |
| 字节偏移落在某段多字节序列内部 | `kInsideUtf8Sequence` | **原值不动** |
| 码元偏移落在代理对内部 | `kInsideUtf16SurrogatePair` | **原值不动** |
| 超过文本长度（终点偏移之内的不算越界） | `kUtf8ByteOffsetOutOfRange` / `kUtf16CodeUnitOffsetOutOfRange` | **原值不动** |
| 出参为空指针 | `kNullOutParameter` | 不写 |

不 clamp、不四舍五入、不"就近吸附到边界"。终点偏移是合法边界（文本末尾的零长度位置）；
空文本有合法的 0↔0 边界，且默认构造的 `Utf8BoundaryMap` **就是**空文本的表，不是未初始化对象。

反方向的一个可判定性质：表里 UTF-16 相邻差只有 1 或 2，因此**任何**严格落在两个码元边界之间的
偏移必然是代理对的下半，所以那个状态名不是猜测而是结论（`e + U+0301` 的表里全是 1，故该文本
没有任何"内部码元偏移"可拒）。

### 5.3 实例：`A U+1F600 e-acute`

实测结果（`build/windows-text-boundary-direct-r1.log`，逐字抄自程序输出）：

```
bytes (UTF-8)   : 41 F0 9F 98 80 C3 A9
units (UTF-16)  : 0041 D83D DE00 00E9
table           : (0,0) (1,1) (5,3) (7,4)
                  ^utf8_byte_offset ^utf16_code_unit_offset
```

即 UTF-8 字节偏移 `0,1,5,7` 对应 UTF-16 码元偏移 `0,1,3,4`。本轮真实断言的拒绝：

| 拒绝的偏移 | 单位 | 返回 |
| --- | --- | --- |
| 2 | UTF-16 码元 | `kInsideUtf16SurrogatePair`（😀 代理对下半） |
| 2 / 3 / 4 | UTF-8 字节 | `kInsideUtf8Sequence`（😀 序列内部） |
| 6 | UTF-8 字节 | `kInsideUtf8Sequence`（`C3 A9` 内部） |
| 8（及 9、10） | UTF-8 字节 | `kUtf8ByteOffsetOutOfRange` |
| 5（及 6、7） | UTF-16 码元 | `kUtf16CodeUnitOffsetOutOfRange` |

其他表的实测值：`中文` → `(0,0) (3,1) (6,2)`；`e + U+0301` → `(0,0) (1,1) (3,2)`；
`U+1D0DA A U+1F4A9` → `(0,0) (4,2) (5,3) (9,5)`；`U+10FFFF` → `(0,0) (4,2)`；
嵌入 NUL `41 00 42` → `(0,0) (1,1) (2,2) (3,3)`。

`e + U+0301` 这一行同时钉住两件事：规范化**没有**发生（两个 scalar 值保持两个），
以及本轮没有字素簇概念（若做了字素聚合，这张表就会少一个边界）。

## 6. 文本身份与版本：本轮的边界，以及它为什么到此为止

一张表只描述建立它的那份文本。实现上做了两件分离的事：

- **已经做到的**：表里只有整数，不保存、不引用调用方的缓冲区，所以表不可能悬垂，
  也不可能被后续修改悄悄改写；构建过程只接受严格转换已通过的文本。
- **本轮不做的**：表无法知道"当初那份文本已经被替换"。跨进程/异步编辑场景需要的文本身份或
  版本校验（例如一个 `text_revision` 号，或一份内容指纹，作为查找的前置条件）
  **尚未实现**，也不该在本轮凭猜测建起来：宿主编辑通知、组合区生命周期和引擎会话都不存在，
  没有任何一方能提供版本号的权威来源。现在加一个自造的计数器，只会造出一个谁都验不了的字段。

因此调用方的当前纪律是：**每次文本变化之后重新构建一张表**，不要把旧表用于新文本。
完整的异步编辑协议属于后续轮次（§9）。

## 7. 明确未实现

以下都不是"藏在别处"，是不存在：

- NFC/NFD 规范化（`NormalizeString` 未被调用）；
- 字素簇光标、字素簇删除、行/词边界；
- 自动大小写（`CharUpper`/`CharLower`/`ToLower` 一类均未使用）；
- `ToUnicodeEx` 键盘翻译、`ITfKeyEventSink`、任何按键处理；
- 宿主文本修改（`ITfComposition` / `ITfTextInput` / `ITfRange` 一个都没碰）；
- 密码字段与隐私级字段判定（`TATTE_*`）；
- 引擎 Session、librime 初始化、C ABI 会话结构；
- 输入法注册、安装、profile/category 登记；
- 文本身份/版本校验（§6）。

## 8. 构建、测试与实际结果

### 8.1 命令

工具链与 P3.1 相同：VS 2022 Professional 自带的 cmake/ctest 3.31.6-msvc6，生成器
`Visual Studio 17 2022`，`-A x64`，单配置 Release。本轮直接按绝对路径调用这两个可执行文件，
因此既不需要 `Enter-VsDevShell`，也不改 PATH（VS 生成器自己用 vswhere 定位工具集）：

```sh
CMAKE="<VS Pro>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
CTEST="<同上目录>\ctest.exe"

"$CMAKE" -S platforms/windows/text -B build/windows-text/test \
      -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
"$CMAKE" --build build/windows-text/test --config Release

# 先证明过滤器匹配非空，再运行（多配置生成器必须带 -C，否则 3 条 Not Run / exit 8）
"$CTEST" --test-dir build/windows-text/test -C Release -N -R "^windows\.text\."
"$CTEST" --test-dir build/windows-text/test -C Release -R "^windows\.text\." \
      --no-tests=error --output-on-failure
```

本机实例路径记在 git-ignored 的 `docs/reports/environment.local.md`，本文档不复制绝对路径。

### 8.2 本轮实际执行与结果

两个提交各自验证后再保存，所以本轮有**两次**独立的配置/构建/CTest 序列：
`build/windows-text/stage1/` 只含编码层与编码相关用例，`build/windows-text/test/` 是最终状态。

| 步骤 | 退出码 | 日志 |
| --- | --- | --- |
| 配置（最终项目，第 1 次） | 0 | `build/windows-text-configure-r1.log` |
| 构建（第 1 次） | **1** | `build/windows-text-build-r1.log`：3 条 `error C3646/C4430` —— 结果结构体在 `Utf8BoundaryMap` 定义完成之前就按值持有它 |
| 构建（第 2 次） | **1** | `build/windows-text-build-r2.log`：44 条错误，根因一条 —— 测试程序少了 `Utf8BoundaryMapBuildResult` 的 `using` 声明，其余全是级联 |
| 构建（第 3 次） | 0，0 warning | `build/windows-text-build-r3.log` |
| 直跑 `--mode encoding` | **5** | 167 checks / **2 failed**：夹具自己写错期望值（见下方说明）。该日志文件后来被同名重跑覆盖，见 §8.2 末的披露 |
| 修正期望值 + marker 诚实化 → 构建（第 4 次） | 0 | `build/windows-text-build-r4.log` |
| 提交 1 前的编码层单独验证 | 见下 | `build/windows-text-stage1-{configure,build}-r1.log` 均 0；`--mode encoding` 167/0、`--mode guards` 11/0、`--mode self-check-fail` 退出 **14**、`--mode boundary` 退出 2（该模式此时还不存在，用法错误才对） |
| 提交 1 的 CTest（3 条） | **0** | `build/windows-text-stage1-ctest-r1.log`，`Total Tests: 3`，3/3 Passed |
| 恢复最终状态后重新配置 | 0 | `build/windows-text-configure-r2.log` |
| 恢复后构建（第 5 次） | 0，0 warning | `build/windows-text-build-r5.log` |
| 直跑 4 种模式（最终） | 0 / 0 / 0 / **14** | `build/windows-text-{encoding,boundary,guards,self-check-fail}-direct-r2.log`：167 / 91 / 12 checks，0 failed；控制条 1 check 1 failed |
| 过滤非空 | 匹配 4 条 | `Total Tests: 4` |
| CTest（最终，4 条） | **0** | `build/windows-text-ctest-r2.log`，4/4 Passed |

`--mode encoding` 第一轮的 2 条失败是**测试自己写错的期望值**，不是实现的错：夹具
`U+1D0DA` 的 UTF-16 码元被手写成了 `D835 DCDA`，而 `F0 9D 83 9A` 对应的正确代理对是
`D834 DCDA`（`D835 DCDA` 是 U+1D11A）。系统两个方向给出的字节 `F0 9D 83 9A`、`F0 9D 93 9A`
互相自洽，说明转换没错、期望值错了。修正后 167/167。独立期望值检查在这里发挥的作用正是
本轮要它发挥的作用：两个方向互相 round-trip 会一致地通过一个错误的手工期望。

**日志覆盖的披露**：构建与 CTest 日志一律用新文件名，没有覆盖任何失败记录。有一处例外——
`build/windows-text-encoding-direct-r1.log`（那条 2 failed 的直跑记录）在我修正期望值并按同名
重跑四个模式时被覆盖了。失败内容与计数在本轮会话输出与本节文字里留了底，但原始文件已不在盘上。
下一轮的同名重跑必须先换后缀。

同一轮里还改掉了测试程序自身的一处不诚实：原先 `RESULT PASS` 是无条件打印的，
于是那份日志读起来像"167 checks 全过"却带着 2 条 FAIL 行。现在只有 `g_failed == 0` 才打印
marker，否则打印 `RESULT FAIL`（`FinishMode()`）。CTest 本来就会被退出码拦住，但日志不该骗人。

### 8.3 四条具名 CTest 用例

判定沿用仓库既有的 `tests/english_completion/assert_exit.cmake`（精确退出码 + marker 策略），
没有新建测试框架，也没有把 Windows 规则塞进英文入口脚本的源码解析里。

| 用例 | 判据 | 检查数 |
| --- | --- | --- |
| `windows.text.encoding_strict` | 退出 0 且日志含 `RESULT PASS` | 167 |
| `windows.text.boundary_map` | 退出 0 且日志含 `RESULT PASS` | 91 |
| `windows.text.length_guards` | 退出 0 且日志含 `RESULT PASS` | 12 |
| `windows.text.suite_can_fail` | 退出 **14** 且日志**不含** `RESULT PASS` | 1（故意断言"截断序列能成功转换"） |

最后一行是可失败性控制条。没有它，前两条是否只是同义反复无从分辨。

### 8.4 非法输入的实测结论

本机（Windows 10.0.26200，SDK 10.0.26100）实测，下列输入全部被拒绝，`GetLastError()` 一律是
**1113 = `ERROR_NO_UNICODE_TRANSLATION`**，因此本层把它们统一分类为 `kInvalidInput` 而不是
`kOsFailure`：

```
非法 UTF-8（15 例）：C3 | E4 B8 | F0 9F 98 | 41 C3 | E4 28 AD | 80 | C0 80 | E0 80 80 |
  F0 80 80 80 | ED A0 80 | ED BF BF | F5 80 80 80 | F7 BF BF BF | FE | FF
非法 UTF-16（7 例）：D83D | DE00 | DE00 D83D | D83D D83D | D83D 0041 | 0041 DE00 | 0041 D83D
```

即：截断、非法延续字节、overlong、UTF-8 编码的代理项、超过 U+10FFFF、孤立高代理项、
孤立低代理项、错误配对——全部拒绝，全部不产出输出缓冲。这两组事实是**测出来的**，
不是从文档记忆里推来的；尤其"UTF-8 编码的代理项在设了 `MB_ERR_INVALID_CHARS` 时会被拒绝"
这一条，若不实测就无法与旧 Windows 的宽松行为区分。

### 8.5 回归

| 回归项 | 命令要点 | 退出码 | 结果 | 日志 |
| --- | --- | --- | --- | --- |
| P3.1 TSF 生命周期 | `ctest --test-dir build/windows-tsf/test -C Release -R "^tsf\.lifecycle\." --no-tests=error` | 0 | 3/3 Passed（过滤器 `-N` 先报 `Total Tests: 3`） | `build/windows-tsf-build-r8.log`、`build/windows-tsf-ctest-r7.log` |
| 英文正式入口 | `tools/test_english_integration.ps1 -EnginePrefix build/librime-english/rime/install -BaselinePrefix build/librime-baseline/rime/install -BuildDir build/english-recovery/test -LogTag p32regress` | 0 | `RESULT PASS 29 case(s) selected, 0 failed, ctest exit 0`（含 `recovery.acceptance.enter_and_space_semantics` 与两条 `english_core`/`infrastructure` 之外的全部正式条目） | `build/windows-text-english-regress-console-r1.log` |

英文入口的日志文件名由它自己的 `-LogTag` 规则生成，不覆盖任何历史失败记录；本轮的控制台抓取另存
到新路径 `build/windows-text-english-regress-console-r1.log`。三项回归全部在**本轮构建成功之后**
运行；P3.1 的生命周期构建只做了一次增量 no-op（exit 0，0 warning）。

## 9. 依赖边界与下一阶段

本轮新增的依赖只有 Windows SDK 的两个 kernel32 导出函数。没有新增第三方依赖，没有链接
`ole32`/`uuid`（那是 P3.1 的测试程序为真实 ThreadMgr 需要的），没有触碰 `engine/`、
`third_party/`、英文核心的依赖边界，也没有把根项目的 `CMakeLists.txt` 或 `CMakePresets.json`
改动一个字符——`build/windows-text/test` 与 `build/windows-tsf/test` 一样，是独立顶层项目。

运行时没有任何网络 I/O（AGENTS.md 第 2 节）；链接的系统库只有 kernel32，函数只有
`MultiByteToWideChar`、`WideCharToMultiByte`、`GetLastError`、`SetLastError`。

接下来要接的是**引擎桥接与宿主组合区**：C ABI 会话结构上的 UTF-16/UTF-8 边界换算调用点、
`ITfKeystrokeMgr` 按键 advise、`ITfComposition`/`ITfTextInput` 的 preedit 与提交，以及
上面 §6 说的文本身份/版本校验——它需要真实编辑通知作为版本来源，所以必须在那一轮之后。
候选 UI 与注册/安装流程在那之后，且注册需要单独一轮的授权与验证。本轮产物即便全部通过，
也仍然不是一个能装上就用的输入法。
