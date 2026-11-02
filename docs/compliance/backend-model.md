# Optional neural reranker provenance and compliance

Status: **isolated optional resource and implementation phase complete; not product admission, release approval, or efficacy approval.**

## Scope and offline boundary

The optional CPU reranker is owned by `engine/neural/`, tested by
`tests/neural_reranker/`, and prepared by `tools/backend/prepare_neural.ps1`. It is
not added to the product root build, does not change the C ABI, and is not wired
into the TSF/product runtime. Generated resources and build outputs remain below
`build/backend-model-acquire/` and `build/backend-neural/`.

The Windows implementation makes no socket calls and the pinned CPU library is
built without the downloader, server, RPC, CUDA, Vulkan, OpenCL, or dynamically
loaded backend options. This is an architectural offline property, not an OS
sandbox guarantee: a normal, non-packaged Windows TSF DLL remains in its host
process and cannot be claimed to be OS-network-sandboxed. The implementation
never sends user content and rejects privacy-denied requests before reading their
content.

## Model and conversion identity

- Model: `Qwen/Qwen2.5-0.5B`
- Official revision: `060db6499f32faf8b98477b0a26969ef7d8b9987`
- Model license observed in the pinned `LICENSE`: Apache-2.0. This is a model
  license, not a product license or a claim that downstream distribution is
  approved.
- Architecture: `Qwen2ForCausalLM`, `qwen2`, BF16 source weights, 494,032,768
  parameters, 24 layers, 14 query heads / 2 KV heads, hidden size 896,
  vocabulary 151,936, training context 32,768 tokenizer tokens.
- Tokenizer: `Qwen2Tokenizer`, BPE, NFC normalizer, Split + ByteLevel pre-tokenizer;
  151,643 vocabulary entries, 151,387 merges, 22 added tokens. `auto_map`,
  `trust_remote_code`, and other remote-code markers were absent. The converter
  was run with no remote model code.
- Downloaded repository files total 999,602,900 bytes across the ten pinned files
  (including `.gitattributes` and `README.md`, which are retained for provenance).
  The eight runtime model/config/tokenizer files used for conversion total
  999,597,531 bytes. The 988,097,824-byte `model.safetensors` matches Hugging Face's LFS SHA-256
  `88c142557820ccad55bb59756bfcfcf891de9cc6202816bd346445188a0ed342`.
  Non-LFS files were checked against their Hugging Face Git blob identities and
  their recorded SHA-256 values.
- Converted local GGUF: `build/backend-neural/models/qwen2.5-0.5b-base-q8_0.gguf`,
  Q8_0, 531,067,776 bytes, SHA-256
  `4b965ca8e4a95be38f3223bbb512cc73112efaef00c0313b7e618f0835c504c0`.
  The resource is below the 512 MiB hard limit (536,870,912 bytes).

The before/after Hugging Face metadata snapshots were identical. The verifier
also checked the model's LFS pointer digest, the expected file set, architecture,
tokenizer metadata, and absence of a nested executable/model-code file.

## llama.cpp source and notices

- Requested tag: `b4514`.
- GitHub tag resolves to commit
  `ec7f3ac9ab33e46b136eb5ab6a76c4d81f57c7f1`.
- SHA-256 of the source archive:
  `21e88c79dbbec249a91b093b56f375a3248a9bbe80c5fb19d42c0849f148aa66`.
- 1,104 ordinary Git blobs in the extracted tree were verified. The archive
  contains seven tracked symlinks; GitHub's tar extraction materializes their
  targets as regular files. That behavior is recorded, not silently treated as
  ordinary Git identity. The Kompute submodule is recorded at
  `4565194ed7c32d1d2efa32ceab4d3c6cae006306` but is not initialized or used.
- Main llama.cpp license: MIT (`source/LICENSE`). The source closure also retains
  `gguf-py/LICENSE` (MIT), `examples/run/linenoise.cpp/LICENSE` (BSD-style), and
  the embedded MIT notice in the CPU llamafile SGEMM source. These notices must
  remain available if this optional library or its source is distributed.
- The Qwen model's Apache-2.0 notice and llama.cpp/embedded notices are separate
  obligations. This phase does not decide the final runtime package notice set.

## Build recipe and implementation boundary

`tools/backend/prepare_neural.ps1` verifies the already acquired resources,
configures `engine/neural/CMakeLists.txt` with the pinned source, builds a static
CPU-only library and test executable, checks the CMake cache, checks the import
table, and runs CTest. It performs no downloads or package installation. The
conversion environment was an isolated Python 3.11 venv using a local,
require-hashes wheelhouse; its verified wheel total was 241,197,424 bytes, below
the 2 GB dependency cap.

The build uses C++17, extensions OFF, MSVC `/permissive- /EHsc`, and only
`GGML_CPU=ON`. `LLAMA_CURL`, `LLAMA_BUILD_COMMON`, `LLAMA_BUILD_EXAMPLES`,
`LLAMA_BUILD_SERVER`, `LLAMA_BUILD_TESTS`, `BUILD_SHARED_LIBS`, `GGML_BACKEND_DL`,
`GGML_RPC`, all GPU backends, BLAS, OpenMP, and Llamafile are OFF. These are
verified cache values, not inferred from a successful configure exit code.

The CMake experiment currently builds the standalone neural target with MSVC
`/MD` so it can be run independently against the existing environment. The
current product/librime baseline uses a different CRT policy (`/MT`); this
standalone experiment is therefore not a packaged/link-integrated library and
must not be represented as one. Any future product admission needs a deliberate
CRT/link boundary decision and a separate rebuild.

The stable interface is `engine/neural/lexical_reranker.h`:

- `Model::Load` accepts only an absolute local fixed-drive GGUF path, checks the
  exact SHA-256 and model metadata, holds read-only model/ancestor handles, and
  rejects paths with name-surrogate reparse points, offline recall flags, wrong
  size, wrong architecture, wrong vocabulary, or wrong parameter count.
- `LexicalReranker::Rerank` takes an already admitted UTF-8 prefix and an immutable
  vector of existing lexical candidate strings. It returns only candidate indices
  and log probabilities; it cannot invent replacement text.
- Defaults are context 512 tokenizer tokens, at most 8 candidates, 32 candidate
  tokenizer tokens, 2 KiB prefix bytes, 128 candidate bytes, four CPU workers,
  and a visible 200 ms request deadline. These are tokenizer-token and UTF-8-byte
  units, not UTF-16 caret units.
- Privacy denial, cancellation, stale version, deadline, invalid UTF-8, malformed
  arguments, and limits return non-OK with no partial ranking. A caller must use
  lexical fallback and re-check its version/permission before applying a result.
- Weights are shared immutable state. Context/KV state is request-local and no
  state file, learning store, telemetry, logging of content, or persistence is
  used. llama logs are routed to a non-null discard callback.

## Verification actually run

Command executed from the existing VS 2022 Developer PowerShell environment:

```text
powershell -NoProfile -ExecutionPolicy Bypass -Command \
  Import-Module ...Microsoft.VisualStudio.DevShell.dll; \
  Enter-VsDevShell ...; \
  & tools/backend/prepare_neural.ps1
```

Observed stage exit codes were all zero:

| Stage | Exit | Observation |
| --- | ---: | --- |
| pinned-resource verification | 0 | Qwen metadata/LFS/blob/file set and llama archive/tree verified offline |
| CMake configure | 0 | CPU-only cache, C++17, extensions off |
| CPU build | 0 | Static `llama`, `ggml`, and `epocanva_neural` test executable |
| import-table check | 0 | No `ws2_32`, `wsock32`, WinHTTP, WinINet, curl, CUDA, or OpenCL imports |
| CTest | 0 | 2/2 tests passed, including intentional failure control |

The real-model test used synthetic public fixtures only; no user text was used.
Observed results:

- model load including SHA verification: 1,039,647 microseconds;
- model metadata: 494,032,768 parameters and 151,936 vocabulary tokens;
- capital fixture order: candidate indices `[1, 2, 0]`, with log probabilities
  `[-0.556706, -5.93801, -18.8177]` in ranked order;
- repeated scoring was deterministic; UTF-8, candidate permutation, privacy-before-
  read, malformed-input, cancellation, stale-version, and active-deadline cases
  passed;
- measured scoring calls were 220,040–225,617 microseconds in the final build;
  peak working set was 596,127,744 bytes;
- the default 200 ms request returned `deadline_exceeded` at 201,496 microseconds
  with an empty ranking. This is the required safe fallback, not a pass of the
  latency target;
- the intentional negative test exercised `Check(false)`, emitted
  `FAIL forced_assertion_failure`, and was accepted only by the exact-exit/marker
  wrapper at exit 1. It is not a `WILL_FAIL` test that accepts arbitrary failures.

## Gates and open decisions

- `default_admission=false`: the optional neural path is not integrated or
  admitted by default.
- `latency_gate_met=false`: observed scoring misses the requested p95 <= 150 ms
  target and the 200 ms cutoff. No AVX2, GPU, candidate-count, or benchmark
  gaming claim is made.
- The neural result is not an efficacy claim, public quality card, or replacement
  for lexical/English behavior. The existing lexical candidate set remains the
  authority; neural ordering is optional and bounded.
- Final runtime distribution still requires a separate notice/legal review for
  Qwen Apache-2.0, llama.cpp MIT, embedded notices, and build-only wheel notices.
- A later phase may investigate CPU performance and integration after the n-gram
  and backend baselines; this phase intentionally does not change the product
  root, TSF, Windows runtime APIs, or C ABI.
