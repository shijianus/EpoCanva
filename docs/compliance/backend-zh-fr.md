# Chinese/French Backend Data Acquisition

This document records the development-stage acquisition and source review only. It is not a product manifest, distribution approval, or legal clearance for the acquired data.

## Scope and ownership

The downloaded source files, upstream API responses, and full license texts are under `build/backend-data-acquire/zh-fr/`. Their exact official HTTPS URLs, pinned commits, tree identities, byte counts, SHA-256 values, license evidence, and dependency notes are recorded in `data/manifests/backend-zh-fr.sources.json`.

The Chinese deployment schemas and importer are now owned by the coordinating implementation. This handoff does not alter those files or claim the importer/deploy test passed. The acquired original files remain unchanged. No nested git repository was created, no packages were installed, and no shared librime engine was rebuilt by this acquisition work.

## Verified upstreams

GitHub's official commit API returned the exact requested full commit objects; recursive tree API responses were complete (`truncated=false`). This fixes content identity but does not authenticate commit authors or establish cryptographic signatures. API responses are retained and hashed under `build/backend-data-acquire/zh-fr/metadata/sources/`.

- Luna Pinyin: `rime/rime-luna-pinyin`, commit `56b934b099dfbeab842320f13aa8b461a6ab3e42`, tree `c0a87ac7e0e6408c9441ba7b6533f3e376c1c627`.
- Terra Pinyin: `rime/rime-terra-pinyin`, commit `723e51bc266cf9464530c1ddedb856aa18e3da34`, tree `ab9bd63a5b3663f21b378ea23572fe938ae0f121`.
- Bopomofo: `rime/rime-bopomofo`, commit `6085c9a38a4a728047862b33d67eee18aa86f3b9`, tree `7c372ce307b3db4f9cd6f4b4e7b2921c077ab5a1`.
- LibreOffice French dictionaries: `LibreOffice/dictionaries`, commit `4f1df64b2e8e14751aa7c2205d170ca016071315`, tree `b5a3d4398ad2f2c33a00b36fc0d28b376c65fb3e`.

## Chinese source and dependency closure

The complete recipe `LICENSE` files from all three Rime repositories are present and byte-identical at 7,651 bytes, SHA-256 `da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768`. The text is GNU LGPL version 3. This evidence does not by itself prove an SPDX `-only` qualifier. Recipe-code licensing is distinct from dictionary-data rights.

The pinned Luna dictionary is 889,896 bytes, SHA-256 `75bcf6eb3ff62b129882ed89cc22b2d80a5347aa72bcfa2ccc839bac298e7314`, with 70,732 data rows after its Rime front matter. Its header cites CC-CEDICT, Android PinyinIME, Chewing, a three-spelling dictionary, OpenCC, and MoeDict without per-source license grants. Its redistribution rights remain unresolved. The main-owned backend currently selects Terra-derived dictionary data rather than Luna.

The pinned Terra dictionary is 1,799,483 bytes, SHA-256 `2f881a239e09a61e5993e79257655f98f81c3789dc8aeea5f33477f710345b27`, with 99,329 data rows after front matter. Its own header declares Creative Commons Attribution-Share Alike 3.0. This is direct source evidence for the declaration, not a substitute for attribution/share-alike compatibility review before distribution.

The pinned Bopomofo schema states `dependencies: [stroke]`, uses `dictionary: terra_pinyin`, `prism: bopomofo`, and includes reverse lookup, simplifier, grammar, custom patch, and custom phrase paths. Its README also names Terra Pinyin as a dependency. The complete pinned `zhuyin.yaml` (1,132 bytes, SHA-256 `77931ab77952fdb2535f8addebc0918453e0220a50ca176380971203f14c48be`) contains four algebra nodes: `pinyin_to_zhuyin`, `free_order`, `abbreviation`, and `keymap_bopomofo`. The input mapping itself has 25 `xform` rules; the other nodes provide four free-order derives, three abbreviation rules, and the key translation `xlit` table. The reduced Zhuyin recipe requires the Terra dictionary and a schema-specific Zhuyin prism generated from its spelling algebra. `stroke`, reverse lookup, OpenCC simplifiers, grammar, custom patches, and custom phrase/user dictionary are not prerequisites for that reduced recipe.

The main-owned importer observed during handoff renames Terra's dictionary resource to `epocanva_backend_terra`, sets `use_preset_vocabulary: false`, verifies there is no `import_tables` declaration, and retains source dictionary rows. This removes the Rime `essay.txt` preset-vocabulary dependency. The pinned librime source confirms this option controls loading preset vocabulary in `third_party/librime/src/rime/dict/dict_settings.cc:55-60` and `dict_compiler.cc:74-76`; the vocabulary resource otherwise defaults to `essay` in `dict_settings.cc`. No conclusion about generated prism/deploy success is made here.

The two current schema IDs observed in main-owned files are `epocanva_backend_pinyin` and `epocanva_backend_zhuyin`. Both set `enable_user_dict: false`, `enable_sentence: true`, and `contextual_suggestions: false`; Rime's pinned `user_dictionary.cc:588-595` confirms `enable_user_dict: false` prevents creation of the user dictionary. This source acquisition does not authorize or add personal learning. Host-side sensitive-field admission still must be enforced independently.

## French source, license, and Hunspell closure

The exact Hunspell pair is:

- `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff`: 199,870 bytes, SHA-256 `c176610cd5dc4846806a65ddd029f422d87978bf58f224aa44222662a16a2de5`.
- `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic`: 1,236,490 bytes, SHA-256 `b78a868e31dd6e373b6c3217969afb898a9acde828a5e7ef97308da42218c88c`.
- `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/README_dict_fr.txt`: 2,758 bytes, SHA-256 `4a25abf740ad6ba5e8d0cfc0d2ffadc3c13c303f8fe389f323b061a389350f83`.

The pinned README identifies French orthographic dictionary version 7.7, explicitly states Mozilla Public License 2.0, and says it requires a Hunspell-compatible checker. The `.aff` header independently repeats MPL 2.0. The repository root has no `LICENSE` at this revision; the repository label is not used as license evidence. The full official MPL-2.0 text was separately acquired from Mozilla at `build/backend-data-acquire/zh-fr/upstream/french/MPL-2.0.txt` (16,726 bytes, SHA-256 `fab3dd6bdab226f1c08630b1dd917e11fcb4ec5e1e020e2c16f83a0a13863e85`).

Source inspection found `SET UTF-8`, `FLAG long`, 18 prefix groups/197 prefix rules, 159 suffix groups/5,184 suffix rules, cross-product headers except non-cross-product prefix group `U.`, and `FULLSTRIP`. The parser/wrapper must preserve Unicode and support `NEEDAFFIX ()`, `FORBIDDENWORD {}`, `CIRCUMFIX **`, `KEEPCASE ||`, `NOSUGGEST --`, `WORDCHARS`, `TRY`, 25 `MAP` groups, 110 `REP` rules, 42 `ICONV` rules, one `OCONV` rule, seven `BREAK` rules, and `KEY`. No `AF`, `AM`, or `COMPOUND*` directives appear in the pinned `.aff` file. Flags are long-mode two-character flag symbols, not single characters or byte offsets. The `.aff` rules include productive and irregular forms; a finite hand-written suffix list is not equivalent.

The `.dic` first line declares 84,782 entries. The file has 84,750 physical lines total and 84,749 following records, including one literal `-` row, a 33-row discrepancy against the header count. Preserve the raw pair and let a verified Hunspell implementation decide file validity; do not equate the count header with physical lines or silently rewrite the source. The direct-wrapper provider receives the exact two paths above. This acquisition does not implement the parser.

## Data and runtime boundaries

The Chinese dictionary row counts above are not corpus sizes, sentence counts, or corpus frequencies. No approved Chinese sentence corpus is provided by this acquisition. The French dictionary is a lexical resource, not a frequency corpus. Data licensing does not inherit the project's AGPL-3.0-only terms.

The source acquisition and importing steps are development-time network operations; the retained build inputs are used offline afterwards. Runtime code must make no network calls. For a normal Windows TSF DLL in an arbitrary host, network exclusion is an architectural property of reviewed code/dependencies, not an OS-enforced sandbox. On Android, network exclusion is platform-enforced only when the application omits `android.permission.INTERNET`; no Android package was built or reviewed here.

No product manifest or package allowlist was modified. Public redistribution is not authorized, and Chinese data rights remain open for legal review.
