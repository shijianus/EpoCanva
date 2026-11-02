# 五语言离线预测后端开发记录（2026-11-02）

## 状态

**已推送工程检查点，新增真实五语言后端与训练评测；尚未满足完整稳定发行要求。** 本轮不新增前端。稳定tag不得由“功能可运行”或少量测试通过推定，`data/manifests/backend-readiness.json`记录尚未完成的语言和发行边界。

用户授权commit、push、tag、release，并批准开发期取得官方词典、模型、语料和项目内工具。依然不授权覆盖其他会话工作、强推、改系统配置或将未达标结果标为稳定。

## 1. 环境及远端

Windows 11 x64，i7-14650HX、16物理核心/24逻辑线程，约31.7GiB RAM；MSVC19.44/VS2022 Professional、SDK10.0.26100.0、CMake3.31.6，Python3.14；模型转换单独使用项目内Python3.11 venv和固定wheelhouse。无系统安装或提权。

- 开发分支：`feat/offline-multilingual-backend`。
- 检查点提交：`4d9bcd3bf65675bf00e053a30f778becba87cab6`，144文件，前轮工程修复及多语言基础。
- 已推送附注预发布tag：`v0.2.7-multilingual-foundation.1`，指向上述提交，不是stable标签。
- `origin`：`https://github.com/shijianus/EpoCanva.git`；实时核对默认分支main，检查点前远端HEAD与本地相同，远端无旧tag。
- 首次push exit0，但现有凭据工具提示TLS验证已关闭；本轮没有修改该设置。后续显式 `GIT_SSL_NO_VERIFY=false git -c http.sslVerify=true ...` 核对远端并推送tag，exit0。
- 获取了官方便携`gh2.103.3`，仅放`build/release-tools/`，SHA256 `98d0cfa794721665d7973dd728c78aff7bd836d58f10f6d7a03568eccb0a22e0`。`gh auth status`真实exit1，未登录；没有提取Git凭据、没有创建GitHub Release网页或上传资产。
- 其他会话的README两处段落、isolation脚本/报告及本地计划保持未提交；使用明确文件清单和README暂存专用内容，没有`git add .`。

## 2. 实际实现

### 统一无前端接口

新增纯C `epocanva_backend.h`，复用原引擎ABI、RimeRuntime、编码与生命周期约束。提供语言session、输入/退格/取消、候选查询/读取/选择、提交读取和应用确认。旧ABI布局不变。

候选与commit使用不同的不可复用token，绑定session/context/generation/catalog。短缓冲不产生半截复制；读commit不学习，完整读取后成功ack才更新上下文和可选ID计数。关闭学习仍允许静态语料预测，个人学习的previous_id独立重置，不能把关闭期间内容写入后来bigram。敏感/Unknown请求拒绝；调用方必须在个人数据读取或学习确认整个调用期间持有真实权限锁，整数context不是OS授权。

资源按哈希绑定并通过本地卷/路径组件句柄固定后读取，防止校验后替换、UNC/mapped网络路径和重解析跳转。原文候选不带学习ID，未能证明来源的Rime文本候选不通过显示内容反推身份。

### 预测

新增静态unigram/bigram/trigram模型、全局/上下文邻接索引、有界四token续写及一次准备的个人统计。静态语料上下文与个人学习分离。版本化TSV包含payload/source哈希、词条ID、计数和一致性校验；缺失/损坏资源拒绝，不联网取得。

词汇服务复用已有英文形态/拼写/分段/缩写核心，增加Windows NFC与大小写查找别名，但不改写模型ID。英文生成词形要求词典收录或真实所有格基词收录。法文重音与韩文词汇补全均来自已准入词汇，而不是固定候选。

### 五语言转换与数据

- 英文：词汇建议、形态、缩写/缩合、二编辑拼写、语料上下文预测及确认学习。
- 中文：真实Terra字典和注音规则，通过Rime script translator产生候选及真实commit；明确禁用user dictionary、custom phrase和隐式history学习。初次无预置词频时罕见同音字领先，使用有归属Tatoeba中文字符计数生成权重修复，没有硬编码你好候选。保留的99,329条reading/surface不变。简繁、词级上下文和完整学习元数据仍未完成。
- 法文：完整Hunspell处理实际`.aff/.dic`，包括重音、复数、变位校验、stem/analyze；不是不完整的自写affix解析器。该数据无完整形态生成标签，Generate空结果如实保留。
- 日文：项目自写bounded k-best Viterbi，实际Mozc1,289,076条reading/surface/POS/cost和2672²连接代价，支持Kana→Kanji和多词路径。非完整Mozc rewriter；部分确认、未知词策略、完整母语质量仍需独立验收。
- 韩文：实际两套式组字，语料词汇补全和next-word；完整词尾/自动空格能力尚未验收。

### 训练来源与划分

实际准备338,406句保留归属的Tatoeba数据：en100,000/fr100,000/zh47,871/ja79,937/ko10,598。CC-BY-2.0-FR作者、句子URL、许可URL随每条记录保留；按已解析base翻译来源家族分组，固定SHA选择和split seed。它不能保证所有语义重复或未记录链接都无泄漏。少量Unicode格式控制记录通过显式开关整条排除，不静默修复文本；仍哈希原始输入。

源清单见`data/manifests/backend-{zh-fr,ja-corpus,hunspell,model}.sources.json`。Terra的CC-CEDICT数据声明CC-BY-SA3.0与仓库LGPL3分开；Tatoeba/Mozc/Hunspell/Qwen各保留自己的许可，不统一贴AGPL。本轮代码push不附加这些数据/模型二进制或未闭合发行资产。

## 3. 真实质量结果

先训练完整计数模型，合计127,597,840字节，超过64MiB总体积目标。随后在再次评测前固定唯一裁剪规则：B/T只保留计数≥2且满足分母约束的记录，U/L不变。原始结果保留，新的train/dev/test/gold/requests哈希与原版逐项相同，未调整测试集或根据测试指标继续调参。

预算版五模型合计 **31,377,210 bytes**，总体积达标。逐语言实际native CLI两次运行（模型与单词频率基线）均exit0，输入/输出/CLI哈希核对一致。下表是该保留测试集、K=5截断评分，不是跨应用真实用户体验保证。

| 语言 | 测试上下文数 | 基线Top3 | 预算模型Top3 | 基线MRR@5 | 预算模型MRR@5 |
| --- | ---: | ---: | ---: | ---: | ---: |
| English | 91,503 | 16.20% | 34.76% | 0.137984 | 0.297143 |
| Chinese（字符级，部分） | 59,487 | 12.69% | 37.92% | 0.099964 | 0.310306 |
| French | 84,884 | 14.54% | 32.37% | 0.126665 | 0.265249 |
| Japanese（字典最长匹配分词） | 80,183 | 15.76% | 41.60% | 0.129252 | 0.351743 |
| Korean | 5,958 | 19.05% | 24.32% | 0.167360 | 0.214225 |

全部预测行status=ok、complete=true；coverage1指请求输出覆盖，**不是词汇OOV覆盖或命中率100%**。按document/base-family重采样的1000次paired bootstrap，MRR提升95%区间下界分别en0.156257、zh0.205271、fr0.135659、ja0.219342、ko0.039896，均为正。预算查询core p95约0.0104–0.0136ms；不等于完整加载/候选生成/磁盘学习/五语言解码端到端p95。

预算版模型SHA256：
- en `51717a82412b7f82abd4bf81a77cd3dc39c0392e184f211c4cad2fe31021a2ad`
- zh `cce13364b25a56940fa1546e738a1571fcd04ed45ee8e91f2ed4ba8ecaa25998`
- fr `e5d6550dcf0da3fb1fefbf8bb7ddd6618dcd6bf3bfd0e2bc50f8c7cbe056b792`
- ja `b40382f5e977d01c40a3feb9d735ffdb72698610b9a41f53a9a55045303c0c77`
- ko `4bb6010cc7145d42361f07e616ceeef3117f1ea7c7a229794442030bd0aa3c31`

### 可选轻量神经模型

取得Qwen2.5-0.5B base固定revision并离线转换Q8_0，531,067,776字节；权重、分词文件、转换器和构建依赖均有哈希。CPU重排器只重新排序已有候选，有取消/过期/超时/隐私拒绝，不保存prompt/token/KV/用户文本。

真实测试2/2通过，但推理约220–226ms，默认200ms截止在约201.496ms返回超时且无部分排名，**延迟准入失败，默认禁用**。观察峰值工作集596,127,744字节。当前是单独`/MD`实验，不与`/MT`主后端链接，未测五语言神经重排质量提升；不是已启用模型增强。

## 4. 命令及实测结果

所有构建产物在`build/`。实际使用现有VS CMake绝对工具路径，以下省略机器前缀；源码不硬编码个人编译器路径。

```text
cmake -S tools/backend -B build/backend-service -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE=<installed VS>
cmake -S tools/backend -B build/backend-service -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON
cmake --build build/backend-service --config Release --parallel 4
ctest --test-dir build/backend-service -C Release --output-on-failure --no-tests=error --output-junit backend-final.xml
python -B tests/backend_training/test_training_evaluation.py -v
python -B tests/backend/test_release_gate.py -v
```

最终后端44/44，零跳过、exit0；训练工具23项单测exit0；门禁缺失资源/越界输出2项测试exit0。早期失败保留：首次未给日文对象`/utf-8`编译exit1；会话隐私回调throw状态顺序不符导致CTest exit8后修复；中文缺真正语料权重使候选测试失败（exit8）后以真实计数修复；日文字典73.9MB误用通用64MiB文件读函数被拒，改为其显式256MiB上限和整文件摘要校验后通过。原生库和源文件未因此改成假成功。

```text
python -B tools/data/import_backend_corpora.py --source-root build/backend-data-acquire/ja-corpus/corpora --output-root build/backend-corpus-prepared --max-per-language 100000
python -B tools/data/import_zh_fr.py --source-root build/backend-data-acquire/zh-fr/upstream/chinese --output-root build/backend-chinese-ranked --corpus build/backend-corpus-prepared/zh.jsonl
python -B tools/data/compile_mozc_lexicon.py --source-root build/backend-data-acquire/ja-corpus/mozc/src/data/dictionary_oss --output build/backend-japanese-data/lexicon.bin
```

三个实际成功exit0；语料曾相对路径/绝对路径混用失败exit1后修复。新增锁定源哈希后重跑语料和日文字典：五个JSONL与旧输出逐字节同SHA，日文资源SHA仍为`e0308bb1e09e33915a40ca487ca3c27081d9564876f655a23fb332e2d6b3d011`。

```text
python -B tools/backend/train_ngram.py --input build/backend-corpus-prepared/<lang>.jsonl --source-kind production-corpus --skip-invalid-records --min-ngram-count 2 --output build/backend-training-budget/<lang>/model.tsv --report build/backend-training-budget/<lang>/report.json --evaluation-requests build/backend-training-budget/<lang>/requests.tsv --eval-cases-out build/backend-training-budget/<lang>/gold.jsonl
python -B tools/backend/evaluate_predictions.py --cases build/backend-training-budget/<lang>/gold.jsonl --requests build/backend-training-budget/<lang>/requests.tsv --model build/backend-training-budget/<lang>/model.tsv --backend-executable build/backend-service/Release/epocanva_backend_cli.exe --output build/backend-training-budget/<lang>/evaluation.json
```

各五语言训练/评测exit0。Japanese另传`--catalog build/backend-japanese-data/segmentation-catalog.jsonl`；完整命令、固定种子、bootstrap和原始/预算结果见`docs/testing/backend-prediction-quality.md`及`build/backend-training-budget/summary.json`。初次真实训练因Unicode格式控制记录拒绝exit1后，增加显式整条排除并记录数量，未改原始语料。

## 5. 尚未完成及稳定门禁

`check_backend_release.py`使用真实JUnit、模型字节与质量报告，默认不放行；当前语言完整性状态和详细原因在`backend-readiness.json`。

- 全语言功能广度、英文Unicode语言学、中文词级/简繁/可信学习metadata、法文完整生成、日文完整segment/rewriter、韩文词尾/空格仍未达标。
- core microbenchmark不代替端到端尾延迟和额外RSS测试。实际五语言预算模型+词典API进程峰值工作集394,973,184字节；这是整进程观察，不是隔离测得的预测层额外RSS，不能据此宣称128MiB额外内存预算通过。
- Chinese字符级/日文最长匹配语料评测有明确语义范围；未完成母语人工复核、完整转换质量基准或跨应用接入。
- 完整数据/模型署名归属包、LGPL relinkable交付和无Git元数据的离线重建仍未验收。
- `gh auth status`未登录导致GitHub Release创建阻塞；已推送Git分支/tag不是已创建Release页面。
- 不创建稳定tag，不把预发布标签移动到新提交，不上传未闭合数据/模型资产。后续代码按新提交正常push，其他会话文件继续保持原状。

Windows普通进程内TSF DLL不存在OS强制网络沙箱；离线保证来自本项目代码与依赖约束、审查和测试。未来Android可通过不申请`android.permission.INTERNET`获得平台强制限制，本轮没有Android发行物。

## 6. 文件清单与最终推送

所有原始语料、词典、模型权重、venv、编译器输出及证据日志都保持git-ignored的build目录；公开推送只含项目代码、测试、recipe和来源/验证文档。下面列出本轮新增路径，其他会话已有修改不纳入提交。

- `data/backend/zh/epocanva_backend_pinyin.schema.yaml`
- `data/backend/zh/epocanva_backend_zhuyin.schema.yaml`
- `data/manifests/backend-hunspell.sources.json`
- `data/manifests/backend-ja-corpus.sources.json`
- `data/manifests/backend-model.sources.json`
- `data/manifests/backend-readiness.json`
- `data/manifests/backend-zh-fr.sources.json`
- `docs/architecture/backend-session.md`
- `docs/compliance/backend-ja-corpus.md`
- `docs/compliance/backend-model.md`
- `docs/compliance/backend-zh-fr.md`
- `docs/reports/backend-prediction-development.md`
- `docs/testing/backend-prediction-quality.md`
- `docs/testing/french-dictionary.md`
- `docs/testing/japanese-decoder.md`
- `docs/testing/prediction-core.md`
- `engine/backend/CMakeLists.txt`
- `engine/backend/backend_api.cc`
- `engine/backend/local_resource.h`
- `engine/backend_session/CMakeLists.txt`
- `engine/backend_session/session.cc`
- `engine/backend_session/session.h`
- `engine/french_dictionary/CMakeLists.txt`
- `engine/french_dictionary/dictionary.cc`
- `engine/french_dictionary/dictionary.h`
- `engine/include/epocanva_backend.h`
- `engine/japanese_decoder/CMakeLists.txt`
- `engine/japanese_decoder/japanese_decoder.cc`
- `engine/japanese_decoder/japanese_decoder.h`
- `engine/lexical_service/CMakeLists.txt`
- `engine/lexical_service/lexical_service.cc`
- `engine/lexical_service/lexical_service.h`
- `engine/lexical_service/text.cc`
- `engine/lexical_service/text.h`
- `engine/neural/CMakeLists.txt`
- `engine/neural/lexical_reranker.cc`
- `engine/neural/lexical_reranker.h`
- `engine/neural/verify_resources.py`
- `engine/prediction/CMakeLists.txt`
- `engine/prediction/model.cc`
- `engine/prediction/model.h`
- `tests/backend/CMakeLists.txt`
- `tests/backend/fixture_model.py`
- `tests/backend/main.cc`
- `tests/backend/real_languages.cc`
- `tests/backend/test_release_gate.py`
- `tests/backend_session/CMakeLists.txt`
- `tests/backend_session/main.cc`
- `tests/backend_training/test_training_evaluation.py`
- `tests/french_dictionary/CMakeLists.txt`
- `tests/french_dictionary/assert_exit.cmake`
- `tests/french_dictionary/main.cc`
- `tests/japanese_decoder/CMakeLists.txt`
- `tests/japanese_decoder/main.cc`
- `tests/lexical_service/CMakeLists.txt`
- `tests/lexical_service/main.cc`
- `tests/neural_reranker/main.cc`
- `tests/prediction_core/CMakeLists.txt`
- `tests/prediction_core/main.cc`
- `tests/prediction_core/make_fixtures.py`
- `tools/backend/CMakeLists.txt`
- `tools/backend/check_backend_release.py`
- `tools/backend/evaluate_predictions.py`
- `tools/backend/main.cc`
- `tools/backend/prepare_french.ps1`
- `tools/backend/prepare_neural.ps1`
- `tools/backend/train_ngram.py`
- `tools/data/compile_mozc_lexicon.py`
- `tools/data/import_backend_corpora.py`
- `tools/data/import_zh_fr.py`

最终提交/push结果在实际命令执行后记录；不能用本报告中的计划替代远端事实。
