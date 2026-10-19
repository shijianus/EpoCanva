# `rime_raw` — dictionary-free input-link test fixture

Schema id: **`epocanva_test_raw`**
Used by: `tools/rime_input_replay` (the only consumer)
Introduced in phase: **P1.3**

## 1. What this fixture is for

It is the minimum configuration that lets a **headless** program prove the real
input chain of the pinned librime build:

```
process_key -> processor -> segmentor -> translator -> Context
            -> get_context (preedit / candidates) -> get_status (schema_id)
            -> commit -> session commit queue -> get_commit
```

Nothing more. It exists so that later phases can say "the link was already
verified" instead of guessing whether a failure is theirs.

## 2. What this fixture is explicitly **not**

| Claim | Status |
| --- | --- |
| English prediction / completion | **未实现** — no model, no n-gram, no plugin is loaded |
| English word selection, morphology, correction, abbreviation expansion | **未实现** — see `AGENTS.md` §1 for what real English support must eventually cover |
| Word list / dictionary | **未使用** — `translator/dictionary` is absent, which is the condition `SchemaUpdate::Run` tests before skipping dictionary compilation (`src/rime/lever/deployment_tasks.cc:349-353`) |
| User learning | **未使用** — no `user_dictionary` translator, no `enable_user_dict`, no sync task is run |
| Unicode / CJK handling | **未验证** — the fixture accepts seven ASCII keys and the probe asserts ASCII text only. A pass here says nothing about non-ASCII input, UTF-8 sequences or grapheme handling |
| A production schema | **不是** — do not ship this. It is test scaffolding, licence status of the whole `build/` tree is still open |

The candidate text this fixture produces is **byte-identical to the keys the test
injected**. It transliterates nothing.

## 3. Component selection — read out of the pinned source, not guessed

Every name below was taken from the `Registry::Register` calls at
`third_party/librime/src/rime/gear/gears_module.cc:46-88`
(librime `1.17.0`, commit `33e78140250125871856cdc5b42ddc6a5fcd3cd4`).

| Role | Component | Why this one, and what was rejected |
| --- | --- | --- |
| letter entry | `speller` (processor) | It is the only registered processor whose accepted-key set is **configurable** — `speller/alphabet` (`speller.cc:74`). `fluid_editor` alone would push *any* byte in `0x21..0x7e` into the input buffer (`editor.cc:58-63`), which would make "only a–z composes" unassertable. |
| BackSpace / Escape / Return | `fluid_editor` (processor) | Registered as `fluid_editor` → `FluidEditor` (`gears_module.cc:48`, `editor.cc:189-203`). Its keymap is exactly the four operations the fixture needs: `BackSpace -> BackToPreviousInput`, `Escape -> CancelComposition`, `Return -> CommitComposition`, and `_auto_commit` is **false** (`Editor(ticket, false)`, `editor.cc:41-44`) so confirming a candidate does not silently commit. `express_editor` was **rejected**: it sets `char_handler_ = DirectCommit` (`editor.cc:216`) and auto-commits, i.e. the first letter would flush the composition. |
| segmentation | `abc_segmentor` (segmentor) | Turns a run of `speller/alphabet` characters into one `"abc"`-tagged segment (`abc_segmentor.cc:39-69`). Rejected: `matcher` (needs `recognizer/patterns`), `punct_segmentor` (needs a punctuator table, which is dictionary-adjacent configuration we do not want). |
| coverage guard | `fallback_segmentor` (segmentor) | Claims each uncovered position as a `"raw"` segment (`fallback_segmentor.cc:16-57`) so that "every input position has a translation source" is structural, not incidental. |
| candidate source | `echo_translator` (translator) | Produces `SimpleCandidate("raw", start, end, input)` at quality −100 and opens **nothing** (`echo_translator.cc:30-43`). This is the whole trick: it is the one registered translator with no dictionary dependency. Rejected: `script_translator` / `table_translator` (both require `dictionary:`), `punct_translator` (requires a punctuator table), `schema_list_translator` / `switch_translator` (switcher internals), `history_translator` (reads commit history state). |
| no filters | `engine/filters` omitted | `CreateComponentsFromList` simply finds no list (`engine.cc:298-326`). Fewer moving parts; `uniquifier` and `simplifier` would only add untested behaviour. |

**No custom component was needed.** The brief asked me to report a real gap if one
existed; there is none for this scope — the five registered components above cover
lowercase-ASCII composition, backspace, cancel and an explicit commit without any
dictionary, model or user data.

## 4. Keys this fixture claims to own

| Key | keysym sent to `process_key` | Consumed while composing | Consumed while idle | Resulting state |
| --- | --- | --- | --- | --- |
| `a` … `z` | `0x61`–`0x7a`, mask `0` | yes (`kAccepted`) | yes (`kAccepted`) | appended to the input buffer |
| BackSpace | `0xff08`, mask `0` | yes, when there is something to pop | **no** | one UTF-8 code unit removed |
| Escape | `0xff1b`, mask `0` | yes | **no** | composition cleared, nothing committed |
| Return | `0xff0d`, mask `0` | yes | **no** | raw composition committed **once** |
| Space | `0x0020`, mask `0` | **no** (binding erased, see schema) | **no** | untouched |
| anything else | — | **no** | **no** | untouched |

Keysym values are from the vendored `third_party/librime/include/X11/keysymdef.h`
(`XK_BackSpace` 0xff08 at :124, `XK_Return` 0xff0d at :128, `XK_Escape` 0xff1b at :132,
`XK_space` 0x0020 at :527). `mask` is the librime modifier bitmask
(`src/rime/key_table.h:15-43`: bit 0 Shift, bit 2 Control, bit 30 Release); the probe
always sends `mask = 0`.

## 5. Units

`AGENTS.md` §5 applies to this fixture and to everything that loads it:

* All text crossing the C API — file paths, `preedit`, candidate `text`, commit
  `text` — is **UTF-8 code units (bytes)**.
* `RimeComposition.length`, `.cursor_pos`, `.sel_start`, `.sel_end` are byte offsets
  **into `preedit`**, not characters and not screen columns.
* `Context::PushInput(char)` and `PopInput(1)` count **bytes of the raw input
  buffer**. For this fixture the raw input is ASCII, so byte == code unit ==
  grapheme, and that equivalence is a property of the *test data*, not a claim
  about the engine.

## 6. Files

| File | Purpose |
| --- | --- |
| `default.yaml` | `config_version`, single-entry `schema_list`, `menu/page_size`. Nothing else. |
| `epocanva_test_raw.schema.yaml` | the schema described above. |

The replay program copies these into a throwaway run directory under `build/` and
deploys them through the public API slots `deploy_config_file` / `deploy_schema`.
They are **never** installed into any real Rime user directory.
