# third_party

本目录存放**已固定版本**的外部依赖工作副本，以及固定它们所用的清单与补丁。

## 当前状态（P1.2）

| 内容 | 状态 |
| --- | --- |
| `third_party/sources.lock.json` | **已存在并已扩展**。除 librime 1.17.0 的发布 tag、完整提交、获取方式、子模块声明与许可证核验之外，P1.2 追加了：四个已取子模块的实际检出与构建情况、Boost 的官方包完整 SHA-256 与许可证核验、`P1_2_build_provenance` 与仍受约束的分发项 |
| `third_party/librime/` | **已获取**的独立 git 工作副本（detached HEAD，提交 `33e78140…`）。被 `.gitignore` 第 44 行 `/third_party/librime/` 精确排除，**不作为普通文件纳入主仓库**。P1.2 构建后 `git status --porcelain` 仍为**空** |
| `third_party/librime/deps/{yaml-cpp,leveldb,marisa-trie,opencc}` | **已按 lock 的 `recorded_commit` 初始化并检出**（`git submodule update --init --depth 1`，**未用 `--remote`**），四个子工作树各自 `status --porcelain` 为空，且都被本轮编译过 |
| `third_party/librime/deps/{glog,googletest}` | **仍未初始化**（`git submodule status` 这两行以 `-` 开头）。这是**刻意的**：`ENABLE_LOGGING=OFF` 去掉 `find_package(Glog REQUIRED)`，`BUILD_TEST=OFF` 去掉 `find_package(GTest REQUIRED)`，本轮既不需要也不构建它们 |
| `third_party/patches/` | **仍然不存在。本轮没有修改任何上游源码，也没有应用任何补丁。** P1.2 遇到的唯一"上游命令行不足"是用 CMake 官方的 `CMAKE_PROJECT_<name>_INCLUDE` 钩子在**构建侧**补齐的，钩子文件在 `tools/cmake/`，见 `docs/build/librime-windows-baseline.md` §10.1 |
| 预编译 DLL / 聚合下载站产物 | **一律不接受**。本轮全部产物都由本项目从固定源码自行编译，落在被忽略的 `build/librime-baseline/` 内 |
| Boost | **不在本目录**。官方源码包解压在 `build/librime-baseline/dependencies/boost/boost_1_89_0`（git 忽略），其身份由 lock 的 `other_required_inputs[0].acquisition` 用完整 SHA-256 固定。上游 `deps/boost-1.89.0` 那个默认位置**没有**被使用，因为把 1.5 GB 解进上游树会破坏"上游字节干净" |

> 说明：P0.1 文档里设想的 `third_party/MANIFEST.md` 已由 **`sources.lock.json`** 取代。
> 只保留一份机器可读清单，避免"两份清单哪个说了算"的问题。

## 1. 引入方式

1. 开发者本人预先取得源码后放入 `third_party/<name>/`，或由仓库内**可离线复现**的脚本
   从**构建机本地缓存**准备。
2. 根 CMake 配置阶段**不得自动联网获取源码**：不允许 `FetchContent`、`ExternalProject`、
   `execute_process(... GIT_REPOSITORY/URL ...)`、也不允许在下载脚本上"先跑起来再说"。
3. 每个依赖都要在 `sources.lock.json` 登记，最小字段集：
   `official_repository`、`release.tag`、`pinned_commit`（40 位完整 SHA）、`acquisition.method`、
   `declared_submodules[]`（path / url / recorded_commit / 是否已获取）、`verification.*`、
   `license_evidence.*`、`generated_at_utc`。
4. 不接受把上游压缩包提交进 Git（体积、可审计性、换行归一化都会出问题）。

## 2. 完整性

- `pinned_commit` + 官方仓库 URL 是可复现锚点；分支名、tag 别名、`latest` 都不算。
- **tag 本身可以被人重新指到别的提交**，所以恢复流程必须先解析 tag，再**比对完整 SHA**。
- 签名/作者身份核验状态必须如实记录。librime 1.17.0 的附注 tag **没有 PGP 签名**
  （`git tag -v` 输出 `error: no signature found`），
  因此 lock 里写的是"身份已固定，完整性未做密码学验证"，
  **不得**把提交 SHA 描述成已完成作者身份或签名验证。
- 升级依赖是一次显式变更：重解析 tag → 更新 lock → 重跑补丁 → 重跑全部测试 → 写阶段报告。

## 3. 按 lock 恢复同一份源码（具体步骤）

以下命令供**任何人**（含干净机器）恢复与 lock 完全一致的工作副本。
本仓库不代跑，因为它们会产生网络访问（属于开发期取源，不是构建期自动下载）。

```sh
# 0) 读取 lock 中的期望值（不要凭记忆）
#    expected_tag=1.17.0
#    expected_commit=33e78140250125871856cdc5b42ddc6a5fcd3cd4
node -e 'const j=require("./third_party/sources.lock.json").packages[0];console.log(j.release.tag, j.pinned_commit)'

# 1) 只取官方地址，按 tag 浅克隆
git clone --branch 1.17.0 --depth 1 https://github.com/rime/librime.git third_party/librime

# 2) 强制校验：解析出的提交必须与 lock 完全一致
cd third_party/librime
git rev-parse HEAD                    # 必须等于 expected_commit
git rev-parse refs/tags/1.17.0^{commit}   # 同上
git describe --tags                   # 期望输出 1.17.0
git status --porcelain                # 期望无输出（工作树干净）
git tag -v 1.17.0                     # 期望 error: no signature found（与 lock 记录一致）

# 3) 子模块：**默认不取**。需要时只按父仓库记录的提交取，禁止 --remote
git ls-tree HEAD deps/                # 读回 6 个 gitlink 及其提交
# P1.2 实际取的是本轮要编译的四个（glog/googletest 不需要，见 lock 的 status_in_p1_2）：
git submodule update --init --depth 1 deps/yaml-cpp deps/leveldb deps/marisa-trie deps/opencc
#   禁止：git submodule update --remote（会漂到分支 HEAD，破坏 lock 语义）
git submodule status                  # 4 行无 '-'/'+' 前缀，2 行（glog/googletest）仍以 '-' 开头

# 3b) Boost：**不是子模块**，且**不需要编译任何库**。取官方包并逐字比完整 SHA-256：
#     https://archives.boost.io/release/1.89.0/source/boost_1_89_0.7z
#     d8b0c1d593d68fe359616aa2fae4b3e258ca1f90ed57eb46c5494c29cdd1d49d
#     解压到 build/librime-baseline/dependencies/boost/，不得放进 third_party/。
#     上游 install-boost.bat 也只跑 `b2 headers`；EpoCanva 连 b2 都不运行。
#     依据见 docs/compliance/dependency-audit.md §5.4 与 docs/build/librime-windows-baseline.md §13.2。

# 4) 恢复后自检：任何一步 SHA 不匹配即停手，不得"先用着"
```

补充约束：

- **按 lock 取回源码 ≠ 能复现本轮构建。** 上面的步骤只还原**源码内容**。基线还依赖
  登记在 lock 的 `epocanva_build_adaptation_inputs` 中的**自有构建适配输入**
  （路径 + 完整 SHA-256）：
  1. `tools/cmake/opencc_external_marisa_includes.cmake`（补 OpenCC 外部 marisa
     缺失的 include/link 目录）；
  2. `tools/build_librime_baseline.ps1`（选项集合与顺序；P1.4 起带
     `-Mode baseline|component-probe`，P2.1 起为
     `-Mode baseline|component-probe|english-completion` 三种显式模式）；
  3. `tools/cmake/epocanva_rime_compiled_in_components.cmake`（**仅两个组件模式需要**：
     把 `EPOCANVA_RIME_COMPONENT_NAMES` 点名的 `engine/rime_components/<name>/`
     编译进 `rime.dll` 的项目侧 CMake 适配；P2.1 起它接受组件列表，
     英文模式据此还带入 `engine/languages/en/` 的语言核心）。
  复现构建必须同时取回相应文件，**不得**声称仅凭上游 SHA 就能完整恢复本次构建。
  第 3 项尤其重要：只按 lock 取源再跑默认构建，得到的是**不含自有组件**的引擎，
  而三种模式的产物目录与 DLL 哈希各不相同，无法混淆。
- 检出后**不得修改**上游文件；改动一律走 `third_party/patches/`（见第 5 节）。
- 若某步不得不为，先记录为阻塞项并报告，不要顺手改上游树。
- 上游的 `build.bat` / `install-boost.bat` / `install-plugins.sh` 等脚本会**下载内容并写入源码树**，
  属未审查的远程取源与就地写树行为；EpoCanva 侧的构建方案不直接调用它们，
  只复用其**参数结论**（见 `docs/build/librime-windows-baseline.md`）。

## 4. 许可证

- 每个依赖必须携带其原始 `LICENSE`/`COPYING` 文件，不得删除、不得改写。
- lock 的 `license_evidence` 记录**在该提交上**取到的许可证文件名、URL、SHA-256 与分类结论。
- **词库/语料的授权与代码授权分离**：librime 检出树内自带 `data/`（约 5.2 MB 最小词库与测试配置），
  其授权在本阶段**未核实**；发布任何词表前必须单独登记并审查。
- **双许可必须显式选择**：marisa-trie 在该提交是 `BSD-2-Clause OR LGPL-2.1-or-later`，
  lock 中记录我们取 BSD-2 分支；若改取 LGPL，链接方式与附带义务都要重新评估。
- Apache-2.0（OpenCC 1.1.9）即使没有 `NOTICE`（在该提交探测为 HTTP 404），
  仍须随分发附带许可证文本、保留版权声明、标注修改。
- 嵌套依赖的许可证若**未核实**，必须在 lock 与 `docs/compliance/dependency-audit.md` 中写明"未核实"，
  不得只复制上游 README 的授权表就宣称全部合规。
- 许可不兼容（例如把某 GPL-only 代码链进闭源输入法核心）必须先出 ADR 决策，不由实现者自行判断。
- **EpoCanva 自身许可证尚未确定**。在架构负责人给出结论前，本目录规则不推导、不暗示主项目
  采用任何特定授权条款，也不要添加 LICENSE 文件。

## 5. 补丁管理

- **禁止就地修改** `third_party/**` 中的上游文件；上游检出必须保持字节干净，以便随时用哈希复核。
- 所有改动以 `third_party/patches/NNNN-<name>-<purpose>.patch` 存放，编号递增，
  并在 `docs/build/bootstrap.md` 的补丁表中逐条说明目的、影响面、撤销方式与所针对的 commit。
- 每个补丁必须可独立应用、可独立回退。
- 若某处改动大到一个补丁难以表达，说明它已不再是"第三方依赖"而应成为自有模块，需要新的 ADR。

## 6. 与 librime 的关系

librime 是第一个登记的依赖。按 `docs/adr/0002-librime-integration-boundary.md`，
它只被 `rime-adapter` 一层链接，其类型与回调不得越过该边界进入平台适配层或引擎门面。
允许源码级改造，但每一处改造都以上述补丁形式记录。
集成面（API、Session、组合状态、组件职责、英文接入位置与不满足项）见
`docs/architecture/librime-integration-map.md`。
