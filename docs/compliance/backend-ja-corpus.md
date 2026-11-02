# Japanese and Multilingual Corpus Source Closure

## Scope and disposition

This record covers the development-time source acquisition under `build/backend-data-acquire/ja-corpus/`, its bounded derived Tatoeba text samples under `build/backend-corpus-prepared/`, and their verified source identities. It does not approve product packaging or public release. No runtime networking was added. No user input, clipboard, or private application data was read. Training was not performed as part of resource acquisition.

The pinned Mozc source is commit `e5f1494923a0a624d01f19c50d57bbdfa03ea5ba`, resolved from the official GitHub API. GitHub marks the commit unsigned. The revision is therefore a fixed source identity, not a cryptographically authenticated author identity. The acquisition tree measured 218,770,940 bytes, below the 500,000,000-byte cap. The machine-readable per-resource paths, byte counts, hashes, endpoint identities, and snapshots are in [backend-ja-corpus.sources.json](../../data/manifests/backend-ja-corpus.sources.json).

## Japanese Kana-to-Kanji resource closure

The acquired Mozc open-source dictionary closure includes ten `dictionary00.txt` through `dictionary09.txt` shards, `connection_single_column.txt`, `id.def`, reading correction, suffix, filtering, suggestion, collocation data, the dictionary build recipe and notices, POS/segmenter rule definitions, and single-Kanji variant rules. The ten shards contain 1,289,076 rows total by physical line count. Each dictionary row is:

`reading<TAB>left_id<TAB>right_id<TAB>word_cost<TAB>surface_form`

The pinned `id.def` has 2,672 rows. Each row maps a numeric ID to POS/comma-separated fields. `connection_single_column.txt` has a first-line width of 2,672 followed by 7,139,584 cost values, giving a 2,672 by 2,672 row-major table. The source proves orientation, not just dimensions:

- `src/data_manager/gen_connection_data.py` at the pinned commit, lines 76-93, sets `rid = array_index // pos_size`, `lid = array_index % pos_size`, and stores `matrix[rid][lid]`.
- `src/converter/nbest_generator.cc`, lines 747-755, invokes `GetTransitionCost(lnode.rid, rnode.lid)`.
- `src/converter/connector.cc`, lines 342-353, reads `rows_[rid].GetValue(lid)`.

Thus the previous node's right ID selects the row and the current node's left ID selects the column. This is actual Mozc source behavior verified at the pinned revision. The physical formats are documented here as text rows and numeric POS IDs; offsets and lengths in this report are file bytes or row counts, not cursor offsets.

Mozc's root `LICENSE` applies its BSD-style notice to project code. Its same-revision dictionary notice is separate: `src/data/dictionary_oss/README.txt` identifies `mecab-ipadic-2.7.0-20070801`, reproduces NAIST permissions/disclaimer and ICOT no-warranty conditions for a large portion of entries, and declares the Okinawa Dictionary public domain. Those statements must accompany any redistributed dictionary data; the project code license must not be treated as a blanket relicensing of the embedded lexicon. The notice also says the OSS set omits Google Japanese Input's large Web-corpus vocabulary and the Japanese postal-code dictionary. It is not the full commercial Google Japanese Input dataset.

## Tatoeba corpora

Official per-language detailed sentence exports were acquired for English (`eng`), French (`fra`), Mandarin (`cmn`), Japanese (`jpn`), and Korean (`kor`). The Mandarin provider code is `cmn`; the attempted `zho` URL returned HTTP 404. The source downloads page documents detailed rows as sentence ID, language, text, username, date added, and date last modified. The corresponding bounded JSONL records retain sentence IDs, language, text, author username, sentence URL, and license URL.

Tatoeba's official downloads page states these text files are released under CC BY 2.0 FR. The terms of use §6.2 says the default text-sentence license is CC BY 2.0 FR and author name citation is required; §6.5 places attribution and license retention responsibility on the reuser. The license terms are attribution obligations, not a grant of authorship authenticity. Tatoeba also warns that submitted content may be invalid, unreviewed, or contributed without the submitter owning rights. The acquired rows preserve Tatoeba's attribution metadata but this work does not independently verify contributor identity or underlying authorship.

The five per-language input dumps total 56,346,161 compressed bytes; `sentences_base.tar.bz2` is 63,963,906 bytes. Together these official Tatoeba exports are 120,310,067 bytes. The `sentences_base.csv` archive member contains sentence ID and base sentence ID: `0` means original, a positive ID identifies the sentence it was translated from, and `\\N` means unknown (rare). The importer only emits records with author attribution and a resolved base-family root. This family grouping reduces direct translation leakage when downstream splits use `document_id`; it does not guarantee separation of semantic duplicates or other links absent from the base relation.

The derived bounded JSONL currently contains 338,406 real source records: 100,000 English, 100,000 French, 79,937 Japanese, 47,871 Mandarin, and 10,598 Korean. Korean is a small-data source and Mandarin/Japanese/Korean remain below the 100,000 sample cap because not enough rows survived the author's-name and resolved-base filters. These are not synthetic fixtures. Their exact hashes, sizes, source counts and exclusion counts are recorded in the adjacent generated `build/backend-corpus-prepared/manifest.json`; the acquisition manifest records its current identity for reproducibility. Each JSONL line contains `id`, `language`, `text`, `source`, `license`, `document_id`, and an `attribution` object with `author`, `sentence_url`, and `license_url`.

A training run is a separate step. N-gram quality, held-out split quality, language balance, and model accuracy are not established by source acquisition or these record counts. The static prediction trainer's tokenizer uses a partial codepoint baseline for Japanese and Chinese when no word catalog is provided; Japanese segmentation and Kana-to-Kanji decoding need the actual dictionary/resource path and their own evaluation.

## Non-scope and licensing boundaries

No CC-CEDICT dictionary was acquired in this task. Mandarin in the Tatoeba bundle is text under CC-BY-2.0-FR, not CC-CEDICT. The repository separately records a Terra Pinyin dictionary with a CC-BY-SA 3.0 data-header declaration and an LGPL v3 recipe/code notice in `data/manifests/backend-zh-fr.sources.json`; those are separate works and obligations, and neither license applies to these Tatoeba sentence snapshots. Existing Rime-derived dictionary sources with CC-CEDICT/MDBG provenance remain subject to their own separate rights review; this acquisition does not clear them. No Google private Web-corpus data, postal data, audio, or unattributed Tatoeba rows were downloaded for these deliverables.

The Mozc resource snapshot, Tatoeba source archive, and derived samples remain under their respective notices/licenses. Do not fold them into EpoCanva's own license or present this provenance record as public-distribution approval. Any future distribution must include applicable Mozc/NAIST/ICOT/Okinawa notices and satisfy Tatoeba attribution for every retained sentence or any permitted derived distribution.

## Verification and remaining limitations

- Commit and recursive tree API responses were saved and hashed; the commit is unsigned.
- The downloaded Mozc dictionary resources matched the pinned tree's API byte sizes; each acquired data file has a SHA-256 in the source manifest.
- The Tatoeba detailed inputs and base export have fixed local SHA-256 values, snapshot `Last-Modified` metadata, and official URLs in the source manifest.
- The bounded JSONL outputs were counted and hashed. No model training or product test is claimed here.
- The importer JSONL encodes `attribution` as an object. The first integration review found a string-only trainer assumption; the training implementation subsequently added lossless structured attribution and tested it. This source-acquisition report does not claim model quality; actual training/evaluation reports record the separate results.
- Attribution usernames are preserved, but are not identity verification. Base-family grouping cannot detect all semantic overlap.
- Korean has 10,598 retained records and is likely insufficient for broad production-quality language claims.
- This source manifest is not a signature and does not prove that a remote provider will keep serving the same snapshot.
- No stable release, product integration, data redistribution approval, or deployment guarantee follows from this acquisition.
