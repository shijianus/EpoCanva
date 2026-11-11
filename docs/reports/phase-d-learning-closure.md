# 阶段 D 报告 — 本地学习 v3、持久化桥接与产品设置

日期：2026-11-06
状态：**部分完成**（存储/迁移/只读偏好/清除已验证；产品持久写入因字段权限锁未证明而保持关闭）

本报告只记录本轮实际执行的命令、退出码和观察结果。未运行的真实宿主验收标为
BLOCKED，不以受控测试代替。

## 1. 仓库与环境

| 项目 | 实际值 |
| --- | --- |
| 工作区 | `D:/Users/Smart Kevin/OneDrive/Desktop/EpoCanva` |
| HEAD | `7442820db35c5bf46ec3a48b57d12a37420d1bff` |
| 分支 | `feat/offline-multilingual-backend` |
| 起始工作区 | 干净；`git status --short` 无输出 |
| 远端 | `origin https://github.com/shijianus/EpoCanva.git` |
| `origin/feat/offline-multilingual-backend` | `7442820`（与本地一致） |
| `origin/main` | `5c0f11b1fa94c965e1a00f920272860c2d20ff73` |
| 工具链 | Windows 10.0.26200 x64，VS2022 Professional，MSVC 19.44.35209.0，CMake 3.31.6-msvc6（VS 随附，不在 PATH），Python 3.14 |
| 本轮未执行 | git commit/push/tag/PR；UAC 提权；TSF 注册/注销；安装/卸载；模型下载；third_party 修改 |

`cmake --preset vs2022-x64-debug` 使用 VS 随附 CMake 绝对路径执行，退出 **0**。

## 2. 独立审计结论（按证据等级）

| 能力 | 等级 | 源码/证据 | 结论与缺陷 |
| --- | --- | --- | --- |
| 学习存储 schema v3 | SOURCE_VERIFIED + UNIT_TESTED | `engine/data_store/learning_store.{h,cc}` | 已实现 profile/language、候选稳定 ID、规范化身份、正负摘要、UTC 毫秒衰减、手动词条、v1/v2 迁移、容量/溢出边界。v2 的“无衰减/无负反馈/无手动词条”已修复 |
| Windows 仓库原子写/恢复 | SOURCE_VERIFIED + UNIT_TESTED | `windows_learning_repository.{h,cc}` | 保留临时文件+刷盘+句柄重命名；新增 `.v2` 迁移备份、`.corrupt` 隔离、`all.lock` 目录级共享/独占锁、`DeleteAll`；损坏/超大/错误版本/错误 catalog 的差异处理 |
| 读取/暂停/清除语义 | UNIT_TESTED | `LearningStore::Read` vs `Record`；仓库 `Gate` | 暂停记录后 `Read`/`LoadSnapshot` 仍允许（Normal+target/generation），写入要求 `learning_enabled`；显式清除不受录制开关影响 |
| 学习事件语义 | SOURCE_VERIFIED + UNIT_TESTED | `engine/ranking/learning_events.{h,cc}` | 无按键/退格事件类型；拒绝/被替代/手动词条；归因与 generation；新增 `SetPreference` 持久化安装器（不伪造事件） |
| 有界个性化排序 | SOURCE_VERIFIED + INTEGRATION_TESTED | `engine/ranking/adaptive_ranker.*`、`engine/backend/backend_api.cc` 的 `ApplyPersonalRanking` | 已对真实 COMPLETION 候选做只读重排；后端顺序作为归一化先验；原文候选保持首位；权重为工程默认值，**未校准**；NEXT_WORD 未接 |
| 真实后端持久化→排序 | INTEGRATION_TESTED | `tests/backend/learning_integration.cc`；`backend.learning.persistence_ranking` | 真实 fixture 候选、真实仓库 v3 文件；偏好改变顺序；重启后保留；`learning_enabled=0` 仍读取；查询不新增持久写；`DeleteAll` 后恢复默认顺序 |
| 五语言真实 profile/磁盘 | INTEGRATION_TESTED | `backend.profiles.real_acceptance` | 真实模型/词典/Rime；按 profile 隔离；磁盘 v3 迁移/重开；暂停读取语义已更新并通过 |
| 产品设置 | SOURCE_VERIFIED + UNIT_TESTED + 受控产品测试 | `product_settings.*`、`settings_window.*`、`product_learning.*` | 录制开关（不可用禁用）、数据路径、确认清除、失败提示；`ProductLearningWritesPermitted()` 恒 false |
| 产品持久写入 | **BLOCKED** | `unified_engine_backend.cc`、`backend_config.h` | 仍固定 `request.learning_enabled=0`；字段权限判定是点查询，不是覆盖排队/worker/确认/持久化的 OS 锁。没有把它伪装成已启用 |
| 真实 TSF 宿主验收 | **BLOCKED** | 当前包/真实宿主 | 本轮未注册、未提权、未做真实宿主会话；不以 mock/受控宿主代替 |
| 公开发行 | **BLOCKED** | release gate | 无 CI、安装器、签名、数据再分发授权；当前包真实宿主闭环未完成 |
| Android / 模型目录 / 28 语言完整实现 | **NOT_IMPLEMENTED** | `platforms/`、`data/manifests` | 不在本轮范围；无新声明 |

审计发现的重复/分叉风险仍然存在：`engine/locale` 与 `tools/backend/stage_resources.py` 的
profile 清单尚未合并；本轮没有把学习 catalog 与 staging profile 清单合并，避免扩大范围。

## 3. D1–D5 实现

### D1 数据模型（v3）

- 文件头：`namespace`、`catalog-sha256`、`profile-id`、`language`（短主语言子标签）、
  `decay-unit=ms`、`half-life-ms`、`revision`。
- `p` 记录：`candidate_id`（稳定逻辑 ID，单 ID 为 `lex:<id>`，多 ID 为 `lex:<id>+...`）、
  `normalized_identity`（UTF-8，≤96 字节，session 做 ASCII 大小写折叠）、
  admitted `lexeme_ids`、positive/negative（千分点）、`last_tick_ms`、`manual`。
- 保留 v2 的 `w`/`b` 行给既有 lexical ranker；v3 是兼容超集。
- 上限保持：2048 word、2048 pair、448 preference、262144 字节；计数/偏好饱和；
  revision 不 wrap；manual 不被普通淘汰驱逐；全 manual 时拒绝新的推断记录。

### D2 迁移、原子写与恢复

- v1/v2 读取后在内存迁移为 v3；仓库先写私有 `.v2` 备份，再原子发布 v3。
- 迁移基线使用调用方当前 tick，不从 Unix epoch 衰减。
- 损坏 v3：返回 `kInvalidFormat`，原文件重命名为 `.corrupt`（有限后缀）；后续写入从空 store 开始。
- 错误版本 `kUnsupportedVersion`、错误 catalog `kCatalogMismatch`、超大文件 `kLimitExceeded` 均保留原文件。
- 命名空间清除删除 `.dat`、`.tmp`、`.v2`、`.v2.tmp`、`.corrupt*`；`DeleteAll` 删除所有 profile 的同类文件；
  不读取损坏内容；没有自动恢复路径。
- 目录级 `all.lock` 让普通事务共享、`DeleteAll` 独占，减少清除与写入的竞争。

### D3 隐私与字段权限

- `Record`/`RecordFeedback`：Normal + `learning_enabled` + target/generation；`Record` 还要
  `host_write_confirmed`；Sensitive/Unknown 在变更前拒绝。
- `Read`/`LoadSnapshot`：Normal + target/generation，不要求 `learning_enabled`；Sensitive/Unknown 拒绝。
- 手工删除：显式用户动作 + Normal，允许在暂停录制时执行；不是新学习。
- 产品路径仍未获得覆盖 worker 全链路的字段锁，因此**不启用持久写入**；这满足任务书
  “如果真实权限锁不能覆盖全过程，保持关闭”的要求。

### D4 学习事件与排序真实接线

- `Session::Acknowledge` 从真实的已确认 commit 生成 `Selection`：
  `candidate_id = CandidateKey(lexeme_ids)`，`normalized_identity` 由 commit 文本有界生成。
- 后端 `Personal()` 读取 repository 快照（暂停也读），把 v3 preferences 通过
  `LearningEventModel::SetPreference` 安装；`AdaptiveRanker` 在 **COMPLETION** 候选上重排。
- 原文候选不进入重排且保持首位；后端顺序归一化为 `[0,1]` 先验；不把未标定的模型原始分
  直接与偏好相加。
- 同一次确认只学习一次：session 在持久化前消费 commit；重复 ack 返回 stale；
  `backend.learning.persistence_ranking` 与既有 `backend.headless.contracts` 覆盖。
- 退格/删除/普通提交没有学习事件类型；结构上不能成为负反馈。

### D5 产品设置

- `ProductSettings` 新增 `learning_recording_enabled`（默认 false，settings.ini 可解析/序列化）。
- `ProductLearningWritesPermitted()` 恒 false；执行层 `learning_writes_trusted=false`，
  `request.learning_enabled=0`；但 `learning_root` 仍传给后端以读取既有偏好。
- 设置窗口新增：
  - “Local learning”状态：说明录制不可用、既有偏好仍影响排序；
  - 禁用的录制复选框；
  - 学习数据路径 `learning-v2`；
  - “Clear local learning data”确认按钮，调用 `DeleteAll(true)`，失败显示原因；
  - 离线/隐私文案不再声称“没有学习”，改为“学习数据留在本机、可清除、无按键日志”。
- `product.integration.deployment_snapshot` 在真实启动事务捕获的配置上断言
  `learning_writes_trusted=false` 与 `learning_recording_enabled=false`，证明封闭门在
  引擎边界仍生效。
- 暂停 vs 清除是两个独立入口；清除不影响基本输入。

## 4. 实际修改文件

新增：

- `docs/adr/0011-learning-v3-and-recording-read-split.md`
- `docs/testing/learning-store-v3.md`
- `docs/reports/phase-d-learning-closure.md`（本文件）
- `docs/task-briefs/phase-e-next.md`
- `platforms/windows/product/product_learning.{h,cc}`
- `tests/backend/learning_integration.cc`

修改：

- `engine/data_store/learning_store.{h,cc}`
- `engine/data_store/windows_learning_repository.{h,cc}`
- `engine/ranking/learning_events.{h,cc}`
- `engine/backend/backend_api.cc`
- `engine/backend/CMakeLists.txt`
- `engine/backend_session/session.cc`
- `platforms/windows/engine_executor/backend_config.h`
- `platforms/windows/engine_executor/unified_engine_backend.cc`
- `platforms/windows/product/product_paths.{h,cc}`
- `platforms/windows/product/product_settings.{h,cc}`
- `platforms/windows/product/product_engine_config.cc`
- `platforms/windows/product/settings_window.{h,cc}`
- `platforms/windows/product/ui_ids.h`
- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/tests/extended_settings_tests.cc`
- `platforms/windows/product/tests/deployment_tests.cc`
- `tests/learning_core/{CMakeLists.txt,main.cc}`
- `tests/learning_repository/{CMakeLists.txt,main.cc}`
- `tests/adaptive_ranking/{CMakeLists.txt,main.cc}`
- `tests/backend/{CMakeLists.txt,main.cc,profiles.cc}`
- `README.md`
- `docs/architecture/overview.md`
- `docs/product/requirements.md`
- `docs/security/threat-model.md`
- `docs/testing/learning-store.md`（标记为 v2 历史）
- `docs/testing/learning-store-windows.md`（标记为 v2 历史）

## 5. 构建与测试（真实命令/退出码）

所有命令均在仓库根目录执行；CMake/CTest 使用 VS2022 随附的 3.31.6 绝对路径。

### 5.1 根工程

```text
cmake --preset vs2022-x64-debug
exit 0

cmake --build build/vs2022-x64-debug --config Debug --parallel 8
exit 0；无编译 warning

ctest --test-dir build/vs2022-x64-debug -C Debug --no-tests=error --output-on-failure
exit 0
100% tests passed, 0 tests failed out of 78
1 explicit skip: learning.repository.symlinks (Win32 1314, 符号链接权限)
```

78 条 = 原有 72 条 + 学习核心 `feedback/decay/manual` 3 条 + 仓库 `corruption/feedback` 2 条 +
排序 `persistence` 1 条。实际执行 77 条通过、1 条显式跳过。

稳定性修正：最终一次根全量曾出现 `learning.repository.errors` 的夹具失败
（`retry after contention succeeds` 得到 io-error）。运行时拒绝 owner/DACL 不是当前
用户的文件是正确行为；根因是测试的 `std::ofstream` 重建叶子后 owner 变为默认 owner。
测试夹具改为写入后恢复当前用户 owner 与受保护的单 ACE DACL；该用例连续 10 次通过，
随后根全量 78/78、exit 0。没有放宽运行时 ACL 检查。

### 5.2 后端

```text
cmake --build build/backend-service --config Release --parallel 8
exit 0；无 warning

ctest --test-dir build/backend-service -C Release --output-on-failure
exit 0
100% tests passed, 0 tests failed out of 58
```

包含真实五语言、真实 profile、Rime、Hunspell、磁盘 v3 迁移/重开与
`backend.learning.persistence_ranking`。

### 5.3 负向验证（关键不变量）

临时在 `ApplyPersonalRanking` 入口加入 `return;`（禁用真实个性化重排）：

```text
cmake --build build/backend-service --config Release --target epocanva_backend_learning_integration
exit 0

ctest --test-dir build/backend-service -C Release -R ^backend\.learning\.persistence_ranking$
exit 8
0% tests passed, 1 tests failed out of 1
FAIL persisted preference reorders the real completion menu
FAIL preferences survive a backend restart and apply while recording is paused
```

删除临时代码后重建：

```text
cmake --build ... --target epocanva_backend_learning_integration
exit 0

ctest ... -R ^backend\.learning\.
exit 0
100% tests passed, 0 tests failed out of 2
```

这证明集成测试确实依赖“持久化偏好影响真实候选顺序”，不是无条件成功。

### 5.4 产品构建与测试

最终目标构建：

```text
cmake --build build/windows-product --config Release \
  --target EpoCanva epocanva_tsf_product epocanva_extended_settings_tests \
           epocanva_learning_tests epocanva_learning_repository_tests --parallel 8
exit 0；无 warning
```

（一次包含不存在的 `epocanva_adaptive_ranking_tests` 目标的调用返回 1；该目标不在产品
构建树中，随后用正确目标列表返回 0。不是代码失败。）

最终针对性回归：

```text
ctest --test-dir build/windows-product -C Release \
  -R "^(learning\.(core|repository)\.|product\.unit\.extended_settings|product\.integration\.(engine_settings|unified_tsf|sink_settings_timing|deployment_snapshot))"
exit 0
100% tests passed, 0 tests failed out of 29
1 explicit skip: learning.repository.symlinks
```

全产品套件运行：

```text
ctest --test-dir build/windows-product -C Release --output-on-failure
99% tests passed, 1 tests failed out of 235
2 explicit skips:
  windows.tsf_context_observer.real_focus
  learning.repository.symlinks
1 failure:
  product.integration.sink_settings_timing
  FAIL timing: the restarted engine is ready
```

该失败是完整套件高负载下的时序抖动：同一测试单独重跑通过，11.51 秒，exit 0。
失败后其余检查（部署记录、marker 退休、输入保真）在同一次运行中仍为 ok。
没有把它隐藏或标记为通过；产品全量仍有 1 个未复现的时序失败，已列为残余风险。

测试计数说明：产品树第一次全量运行时，5 条学习测试因测试可执行文件未随新源码重建而失败；
重建 `epocanva_learning_tests`/`epocanva_learning_repository_tests` 后 24/24 学习测试通过。
第二次全量运行只剩上述 `sink_settings_timing` 时序失败。

## 6. 产品链路结论

- **学习事件来源**：`backend.learning.persistence_ranking` 使用真实后端候选（fixture 模型的
  `walk`/`world` 等）、真实 commit/selection 路径；不是固定候选数组。
- **持久化**：v3 文件由 `WindowsLearningRepository` 写入；重启后端后读取同一文件。
- **排序影响**：持久化偏好使真实 COMPLETION 菜单中的目标候选越过被替代候选；
  原文候选仍第一。
- **暂停**：`learning_enabled=0` 的 session 仍读取既有偏好并得到相同顺序；查询没有新增
  持久状态（测试断言 preferences 数量不变）。
- **清除**：`DeleteAll(true)` 后同一真实菜单恢复默认顺序，仓库快照无偏好；清除不是
  “只是停止写入”。
- **产品设置**：设置页可显示数据路径、说明不可用、执行清除；`ProductLearningWritesPermitted()`
  为 false，因此**产品不会写入新学习**。这是本轮唯一未完成的产品接线门槛。

## 7. 隐私与安全

- 敏感/未分类字段在 `Read`、`LoadSnapshot`、`Record`、`RecordFeedback` 的入口拒绝；
  没有原始击键、preedit、上下文、窗口标题或字段名进入 v3 文件。
- 持久化的 `normalized_identity` 是候选词形（可能包含用户输入），按个人数据处理；
  `.dat`、`.v2`、`.tmp`、`.corrupt` 都在私有目录并受显式清除覆盖。
- 迁移备份和损坏隔离不会被自动读取，清除后不能“复活”偏好。
- 目录/文件 DACL 是访问控制，不是加密；不声称抵御已控制设备的恶意软件。
- **残余风险**：产品读取路径的 `PrivacyCallback` 仍是入队时的 access 快照；TSF 字段
  权限不是 OS 锁。产品写入因此关闭；读取只影响当前查询，焦点/代际变化由协调器丢弃结果。
  真实宿主验收仍未执行。

## 8. 未完成 / 阻塞 / 风险

| 项目 | 状态 |
| --- | --- |
| 产品持久写入（TSF 字段锁全链路） | **BLOCKED**，保持关闭 |
| 当前包真实 TSF 宿主验收 | **BLOCKED**（UAC/会话授权） |
| 排名权重校准与离线评测 | NOT_IMPLEMENTED（工程默认值） |
| NEXT_WORD 个性化 | 未接（本阶段有意保持上下文模型顺序） |
| 旧 `backend.<lang>.v1` 命名空间自动导入 | 未实现；产品从未写入，仓库级 v1/v2 迁移已验证 |
| 中文 Rime 菜单候选的学习身份 | 无 admitted lexeme ID 的 Rime 文本只用于瞬时排序，不学习；有模型 ID 的简拼候选可学习 |
| 产品全量 `sink_settings_timing` 时序抖动 | 残余风险；单独重跑通过，未稳定复现 |
| 设置窗口高 DPI 布局 | 新增“本地学习/学习数据”分组在 125%+ DPI 下的完整可见性未做真实显示验收 |
| Android / 模型目录 / 28 语言完整实现 | NOT_IMPLEMENTED |
| CI、安装器、签名、公开数据授权 | BLOCKED / NOT_IMPLEMENTED |
| 电源丢失注入、真实宿主矩阵 | 未执行 |

## 9. 总体结论

EpoCanva 在本轮获得了可证明的真实能力：

- 一个 schema v3 的本地学习存储，带 profile/language 身份、衰减、负反馈、手动词条、
  v1/v2 迁移、原子写、迁移备份、损坏隔离和跨 profile 清除；
- 一个真实的后端持久化→排序闭环：持久化偏好改变真实 COMPLETION 候选顺序，重启后保留，
  暂停记录仍可读；
- 一个诚实的产品设置入口：不可用的录制开关、数据位置、确认清除，且不伪造“已启用学习”；
- 既有编辑/候选/五语言后端测试未退化（根 78、后端 58、产品定向 29 均 exit 0）。

仍然不可用/未证明：

- 产品不会在真实 TSF 宿主中持久学习（字段锁未证明，写入关闭）；
- 当前包没有新的真实宿主输入/候选/卸载闭环；
- 排序权重未校准，NEXT_WORD 未个性化；
- 没有 Android、模型目录、CI、安装器或公开数据授权。

因此本轮完成定义按任务书允许的“部分完成”报告，而不是宣称产品学习已经交付。
