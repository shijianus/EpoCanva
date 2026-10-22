# Windows TSF 引导（P3.1）：不注册的 COM 生命周期骨架

本文档描述 `platforms/windows/tsf/` 这一层**当前实际是什么**、如何配置构建、如何验证，
以及它**不是什么**。P3.1 的边界是生命周期与卸载判断；输入能力不在本轮范围内。

## 1. 这一轮是什么，不是什么

是：

- 一个 x64 的 TSF 文本服务 DLL（`epocanva_tsf`），导出**且只导出**
  `DllGetClassObject` 与 `DllCanUnloadNow`；
- 一个 `IClassFactory` 实现（含 `LockServer`）；
- 一个 `ITfTextInputProcessorEx` 实现（`Activate` / `ActivateEx` / `Deactivate`）；
- 一个通过绝对路径受控加载该 DLL、并在**系统真实 ThreadMgr** 上跑激活/停用的验证程序。

不是：

- **不是已安装的输入法**。没有 `DllRegisterServer`、没有 `DllUnregisterServer`、没有
  profile/category 注册、没有安装器、没有注册表写入。任何一次运行都不需要管理员权限。
- **没有键盘处理**。没有按键 sink、没有 `ITfKeystrokeMgr` AdviseKeyboardEvent、没有
  `OnTestKeyDown`/`OnKeyDown`。
- **没有候选窗**。没有 `ITfComposition`、没有 `ITfTextInput`、没有 UI 候选列表、没有
  宿主文本编辑。
- **没有引擎接入**。没有 librime 初始化、没有 IPC、没有引擎服务；`engine/` 与
  `third_party/` 不被本轮任何目标引用。

因此"生命周期测试全绿"只证明引用计数、模块计数与卸载判断是对的，**不**证明这个 DLL
能打字。文档与测试输出都不做任何相反的暗示。

## 2. 目录与目标

```
platforms/windows/tsf/        独立顶层 CMake 项目（不是根项目的子目录）
  CMakeLists.txt              项目、CRT、目标定义
  epocanva_tsf.def            导出表（两个名字，均标 PRIVATE）
  dll_main.cc                 DllMain + 两个导出入口 + 异常边界
  module_state.{h,cc}         模块活对象计数 / 服务器锁计数
  class_factory.{h,cc}        IClassFactory
  text_service.{h,cc}         ITfTextInputProcessorEx
  identifiers.{h,cc}          冻结的 CLSID
tests/windows/tsf_lifecycle/  验证程序与 3 条具名 CTest 用例
  CMakeLists.txt              被上面的项目 add_subdirectory 进来
  main.cc                     lifecycle / threadmgr / self-check-fail 三种模式
```

`platforms/windows/tsf/CMakeLists.txt` 顶部有守卫：它**必须**作为自己的顶层项目配置。
理由写在文件里——CRT 选择不能被根项目覆盖，且这一轮不需要重建引擎和第三方依赖。
`tests/windows/tsf_lifecycle/CMakeLists.txt` 也有守卫：它不能被单独配置，因为没有 DLL 可验。

CRT：`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`（静态 `/MT`，Release 配置）。
这个选择的可见结果是 `epocanva_tsf.dll` 的导入表里**只有 `KERNEL32.dll`**，没有任何
CRT/VCRUNTIME 的 DLL 依赖，因此把它装进宿主进程不需要先装 redistributable。
验证程序用同样的设置；两者之间不共享 CRT 状态（程序只经 `LoadLibrary` +
`GetProcAddress` + COM 接口触碰 DLL）。

## 3. 配置、构建、测试

在仓库根目录执行。本轮的构建目录是独立的 `build/windows-tsf/test`，
与英文集成测试的 `build/english-recovery/test`、以及根项目的 `build/vs2022-x64-debug`
互不影响。`cmake` / `ctest` 取自 VS 2022 Professional 自带的那一套（3.31.6-msvc6）；
本机实例路径记在 git-ignored 的 `docs/reports/environment.local.md` 里，本文档不复制绝对路径。

```sh
# 1) 配置（独立项目 + x64 + 单配置 Release）
cmake -S platforms/windows/tsf -B build/windows-tsf/test \
      -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_CONFIGURATION_TYPES=Release

# 2) 构建
cmake --build build/windows-tsf/test --config Release

# 3) 先证明过滤器匹配非空（多配置生成器必须带 -C，见下）
ctest --test-dir build/windows-tsf/test -C Release -N -R "^tsf\.lifecycle\."

# 4) 运行
ctest --test-dir build/windows-tsf/test -C Release -R "^tsf\.lifecycle\." \
      --no-tests=error --output-on-failure
```

`-C Release` **不是可选的**。生成器是 `Visual Studio 17 2022`（多配置），测试命令用
`${CMAKE_COMMAND} -P assert_exit.cmake` 间接拉起 exe；漏掉 `-C` 时 CTest 会报
`Test not available without configuration`，三条用例全部 `Not Run`，退出码 **8**。
本轮真实踩过这一步，记录留在 `build/windows-tsf-ctest-r4.log`（失败记录未被覆盖）。

三条具名用例：

| 用例 | 判据 |
| --- | --- |
| `tsf.lifecycle.com_lifecycle` | 退出 0 且日志含 `RESULT PASS` |
| `tsf.lifecycle.real_threadmgr_activation` | 退出 0 且日志含 `RESULT PASS` |
| `tsf.lifecycle.suite_can_fail` | 退出 **14** 且日志**不含** `RESULT PASS`（可失败性控制条） |

判定沿用仓库既有的 `tests/english_completion/assert_exit.cmake`（精确退出码 + marker 策略），
没有新建测试框架，也没有把 Windows 规则塞进英文入口脚本的源码解析里。

### 3.1 本轮实际执行与结果

| 步骤 | 命令要点 | 退出码 | 日志 |
| --- | --- | --- | --- |
| 配置 | `cmake -S platforms/windows/tsf -B build/windows-tsf/test -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release` | 0 | `build/windows-tsf-configure-r3.log` |
| 构建 | `cmake --build build/windows-tsf/test --config Release` | 0（0 warning、0 error） | `build/windows-tsf-build-r6.log` |
| 过滤非空 | `ctest ... -N -R "^tsf\.lifecycle\."` | 匹配 3 条 | 控制台 |
| 运行（漏 `-C`） | `ctest ... -R ... --no-tests=error` | **8**，3 条 Not Run | `build/windows-tsf-ctest-r4.log` |
| 运行 | `ctest -C Release ... --no-tests=error --output-on-failure` | **0**，3/3 Passed | `build/windows-tsf-ctest-r5.log` |
| lifecycle 直跑 | `tsf_lifecycle.exe --dll <绝对路径> --mode lifecycle` | 0，**51 checks / 0 failed** | `build/windows-tsf-lifecycle-direct.log` |
| threadmgr 直跑 | 同上 `--mode threadmgr` | 0，**16 checks / 0 failed** | `build/windows-tsf-threadmgr-direct.log` |
| 导出表 | `dumpbin /exports Release/epocanva_tsf.dll` | 2 functions / 2 names：`DllCanUnloadNow`、`DllGetClassObject` | 控制台 |
| 依赖 | `dumpbin /dependents` | DLL：`KERNEL32.dll`；验证程序：`ole32.dll`、`KERNEL32.dll` | 控制台 |

工具链（配置日志自陈）：MSVC 19.44.35209（工具集 14.44.35207）、Windows SDK 10.0.26100.0、
目标 OS 10.0.26200、x64。

## 4. 接口与生命周期契约

这些是本轮**已实现并且已被测试**的行为。

1. `QueryInterface` 的支持集是封闭清单。类工厂：`IID_IUnknown`、`IID_IClassFactory`。
   文本服务：`IID_IUnknown`、`IID_ITfTextInputProcessor`、`IID_ITfTextInputProcessorEx`。
   清单外的 IID 返回 `E_NOINTERFACE` **且把出参置空**，调用方手里不会留下需要释放的东西。
2. `IID_IUnknown` 与其余接口返回同一地址（单继承链），IUnknown 身份一致——测试直接比地址。
3. `AddRef`/`Release` 用 `Interlocked` 系列；宿主线程不受我们控制，所以不能假设单线程调用。
   对象通过 `Release()` 归零自我销毁，析构函数私有，`delete` 在编译期就不可达。
4. 不支持聚合：`pUnkOuter != nullptr` 一律 `CLASS_E_NOAGGREGATION`，且出参保持为空。
   委派 IUnknown 的路径不存在，装作存在只会造出一个算术会撒谎的对象。
5. 模块活对象计数覆盖类工厂与文本服务两者；`DllGetClassObject` 里"造出来应答查询、
   又被查询拒绝销毁掉"的那条路径同样计数进出成对（测试用第 3.2 节的两条卸载判断夹住它）。
6. `LockServer` 计入模块锁计数，与对象计数相互独立。锁没放完时 `DllCanUnloadNow` 必须是
   `S_FALSE`；无锁可放时第二次 `LockServer(FALSE)` 返回 `E_UNEXPECTED` 且**不把计数写成负**
   （一旦下溢，之后的卸载判断会永久停在 `S_FALSE`，看起来像泄漏）。
7. 所有对象与所有锁都释放后 `DllCanUnloadNow` 返回 `S_OK`。
8. 导出函数不穿越 C++ 异常：`DllGetClassObject` 把 `bad_alloc` 译成 `E_OUTOFMEMORY`、
   其余译成 `E_FAIL`；`DllCanUnloadNow` 不分配、不调用任何可抛出的东西，所以不设 `try`。
9. 不跨 DLL 边界传 STL：两个导出的参数只有 GUID / IID / 指针。

### 4.1 激活路径

`Activate` 与 `ActivateEx` 共用同一个内部实现 `ActivateInternal`：`Activate` 就是
`ActivateEx(thread_manager, client_id, 0)`。这个关系是 SDK 头文件本身定义的——
`ITfTextInputProcessorEx`（`msctf.h:10595-10596`，uuid `6e4e2102-f9cd-433d-b496-303ce03a6507`）
继承 `ITfTextInputProcessor`（`msctf.h:10500-10501`，uuid `aa80e7f7-2021-11d2-93e0-0060b067b86e`），
只多一个 `ActivateEx`。

本轮激活只做五件事：参数校验、持有 `ITfThreadMgr` 引用、记录 client id、记录激活标志、
`Deactivate` 时的对应清理。行为如下，均由测试钉住：

| 情形 | 本项目契约 | 依据 |
| --- | --- | --- |
| `thread_manager == nullptr` | `E_INVALIDARG`，**在改动任何状态之前返回** | 已有激活不会被一次失败的激活拆掉 |
| 正常激活 | `S_OK`，持有 thread manager 引用 | `Deactivate` 归还 |
| 重复激活（仍处激活态） | `S_OK`：先走完整 `Deactivate` 再激活 | 自愈，绝不双重持有引用 |
| 停用 | `S_OK` | 释放引用、清零 client id 与 flags |
| 未激活时停用 | `S_OK`，无副作用的 no-op | **这是我们选的契约**（本项目自定义，TSF 不会自发产生这个序列） |
| 激活后再停用再激活 | `S_OK` | 交替可用 |
| `TfClientId` 的取值 | **原样存储，不校验** | `msctf.h:1027`：`TfClientId` 是普通 `DWORD`，那个 uuid 只用于 marshal，头文件没有标注任何非法值；在这里"校验"等于编造规则 |

诚实边界：COM **不向外暴露引用计数**，所以"是否在宿主对象里漏下一个引用"从客户端一侧
观察不到。本项目能观察并钉住的是我们自己的算术——模块计数、卸载判断、以及释放顺序；
析构走同一个 `Deactivate` 路径而不是第二套清理逻辑，正是为了让这条不可观测的路径
只有一个实现可能出错。

## 5. DllMain 与"不注册"

`DllMain` 只做一件事：`DLL_PROCESS_ATTACH` 记下 `HINSTANCE`，`DLL_PROCESS_DETACH` 清空。
不初始化 COM、不初始化引擎、不建线程、不扫文件、不加载语言包、不起日志设施。

`DisableThreadLibraryCalls` **没有调用**。它是一项针对"知道自己线程生命周期"的 DLL 的
优化，而加它的前提是核实静态 CRT 在 `_DllMainCRTStartup` 里的簿记依赖——本轮没有做这个
核实，而当前骨架既不创建也不销毁线程，加了省不到任何东西，却会把我们绑在一个未验证的
CRT 假设上。AGENTS.md 要求核实后再动，所以这里保持不动。

`epocanva_tsf.def` 是唯一的导出机制：源码里没有任何 `__declspec(dllexport)`，
所以导出表和 .def 不可能漂移。`DllRegisterServer` / `DllUnregisterServer` 是**不存在**，
不是"存在但返回失败"——不存在的导出没法被 regsvr32 误调。验证程序从导出表断言两者缺失。

两个名字标了 `PRIVATE`：这是给 `GetProcAddress` 和 COM 装载器用的入口，没有任何东西
链接这个 DLL 的导入库；不标 PRIVATE 时链接器报 `LNK4104`（本轮早期构建日志里真实出现过，
加上 PRIVATE 后为 0 warning）。PRIVATE 不影响 `GetProcAddress` 按名解析。

GUID 一次性生成并**固化在源码**（`identifiers.cc`，`{B440F702-63E4-4D47-BCC4-074899ABA836}`），
不参与构建期生成。测试程序编译 DLL 的同一份 `identifiers.cc`，所以仓库里这个 CLSID
只有一个定义，测试不会对着一个漂移副本验证。测试自己另外冻结了两个只用于探测"未知/不支持"
的 GUID（`main.cc` 顶部注释），它们不对应任何实现。

## 6. 测试程序如何验证（以及一处它原先的错）

程序只通过命令行给定的**绝对路径**加载 DLL：`LoadLibraryW` → 比对 `GetModuleFileNameW`
与 `GetFullPathNameW`（证明加载的确实是这个文件，而不是搜索顺序撞上的另一份）→
`GetProcAddress` 取两个入口 → 之后只经 COM 接口交互。全程不依赖注册表里有 EpoCanva CLSID。

`lifecycle` 模式覆盖的卸载判断序列（51 条检查里的关键 8 条，按实际输出顺序）：

```
freshly loaded, no object created            → S_OK
after the refused CLSID / refused IID        → S_OK   （被拒的调用不留活对象）
factory alive                                → S_FALSE
service alive, factory gone                  → S_FALSE
all objects released                         → S_OK
factory and lock both alive                  → S_FALSE
no object alive, one lock held               → S_FALSE   ← 锁与对象生命周期分离
lock released and every factory gone         → S_OK
```

其中第 7 条是本轮分离出来的：只说"有锁就不可卸载"是不够的，因为工厂本身还活着时，
`S_FALSE` 有两个可能原因。这里的顺序是**先放掉工厂的最后一个引用、再读判断**，此时模块里
一个对象都不剩，`S_FALSE` 只能来自锁。放锁则通过**新取一个工厂实例**调 `LockServer(FALSE)`
——锁是模块状态（`module_state.cc` 的两个计数器），不是对象状态，任何同类工厂实例都是
合法调用者。

这一条也顺手改掉了一份草稿里的真实缺陷：原先的写法在 `factory->Release()` 归零销毁对象之后，
又用同一个指针调 `LockServer`。那是 use-after-free，而且在 Release/静态 CRT 下**大概率仍然通过**
——被释放内存里的 vtable 槽通常还没被覆盖，于是测试照样打印 `RESULT PASS`。这正是
"一个不可能失败的测试不是测试"的反面变体：一个**会假绿**的测试。现在的序列不依赖任何
已销毁的指针，同时把锁的效应变得真正可分离（若 `LockServer` 是按对象记账的，
"no object alive, one lock held" 就会翻成 `S_OK` 并当场失败）。

程序在 `FreeLibrary` 之后不触碰任何遗留指针；卸载判断一律在 free **之前**取。

`threadmgr` 模式（真实 TSF，不是 mock）：`CoInitializeEx(APARTMENTTHREADED)` →
`CoCreateInstance(CLSID_TF_ThreadMgr, ...)`（`msctf.h:783` 声明、由 `uuid.lib` 定义）拿到
**系统的 ThreadMgr** → `ITfThreadMgr::Activate` 取应用 client id →
QI `ITfClientId`（`msctf.h:10700-10705`，uuid `d60a7b49-1b9f-4be2-b702-47e9dc05dec3`）后
`GetClientId(我们的 CLSID)` 取服务侧 client id → 用 DLL 自己的 `DllGetClassObject` 造服务
对象 → 对这个真实 manager 做 Activate / 重复 ActivateEx / Deactivate / 再 Deactivate /
再 Activate / 失败激活 / 最终 Deactivate → 按序释放 → `CoUninitialize` → `FreeLibrary`。

本轮真实取值：`app=32 service=59`（这两个数字是**这一个进程内这一个 ThreadMgr** 的内存 id，
不是注册表里的东西，也不跨进程有意义）。`GetClientId` 只做 CLSID→内存 id 的映射，
不写 profile、不写注册表、不改输入语言列表。任何阶段失败都会打印阶段名与 HRESULT 并以 6
退出——环境不支持时它报告自己，不会被折成一次通过。

`self-check-fail` 模式故意断言"未知 CLSID 能拿到工厂"，必须以 14 退出且不打印
`RESULT PASS`；没有这条，上面两组断言是否只是同义反复无从分辨。

## 7. 明确未实现

以下都不是"藏在别处"，是不存在：

- `DllRegisterServer` / `DllUnregisterServer` / 任何注册表写入；
- TSF profile、category（`CATID_TextService_*`）注册，`ITfTextInputProcessor` 之外的任何
  TSF 接口实现（`ITfKeystrokeMgr` advise sink、`ITfTextInput`、`ITfComposition`、
  `ITfDisplayAttributeProvider`、`ITfFn*`）；
- 按键处理、组合/preedit、候选 UI、宿主文本读写；
- UTF-16 ↔ UTF-8 偏移换算（AGENTS.md 第 5 节要求它在 C ABI 边界定义，尚未定义）；
- librime 初始化、引擎会话、IPC/引擎服务、跨应用一致性；
- 字段隐私等级识别（`TATTE_*`）。

`docs/architecture/overview.md` 第 5 节的状态表已同步为"生命周期骨架已实现、输入能力未实现"。

## 8. 系统变更边界

本轮执行过程中**没有**：regsvr32、注册表写入、输入法 profile/category 注册、安装器、
提权、全局 PATH 修改、任何网络 I/O 或遥测（AGENTS.md 第 2 节）。链接的系统库只有
`ole32`（测试程序的 `CoInitializeEx`/`CoCreateInstance`）与 `uuid`（`CLSID_TF_ThreadMgr`
等常量定义），产物只在 `build/windows-tsf/` 下生成。

## 9. 下一阶段

接入的是**输入桥接**，不是把这份骨架补完：C ABI 边界上的 UTF-16/UTF-8 与 caret 单位换算、
引擎会话与生命周期、`ITfKeystrokeMgr` 的按键 advise、`ITfTextInput`/`ITfComposition`
的 preedit 与提交。候选 UI 与注册/安装流程在那之后，且注册需要单独一轮的授权与验证——
本轮的产物即便全部通过，也仍然不是一个能装上就用的输入法。
