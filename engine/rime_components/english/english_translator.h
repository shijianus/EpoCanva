// EpoCanva P2.1 -- the English Translator, and the C++ side of its diagnostics.
//
// INTERNAL TO THE ENGINE BUILD except for english_diagnostics.h: a caller outside
// rime.dll includes english_diagnostics.h only. The C++ declarations here depend
// on librime's private headers at the pinned commit and are not a front-end SDK
// (docs/adr/0004-compiled-in-rime-components.md, 「限制」 1).
//
// What this class does: it turns the current segment's input into candidate 0 =
// the original text, followed by up to 7 completions read from a word list
// compiled into the same PE module's heap. Whether the input is a queryable English
// token at all -- letters, at most one internal apostrophe or hyphen, no technical
// mark -- is decided in engine/languages/en/english_token.h, not here; this class
// only counts the answer it got. Everything linguistically interesting is
// delegated to engine/languages/en/, which knows nothing about Rime.
//
// What this class deliberately does NOT do:
//  - it does not derive from rime::Memory, so no dictionary, no user dictionary,
//    no language plugin and no commit-driven learning is attached to it;
//  - it reads no file during Query(); the index is acquired once, in the
//    constructor, from a process-wide cache keyed by the resolved path;
//  - it emits no log statement. Query() receives the user's composing text, so a
//    single LOG() here would put composition content in a log file
//    (AGENTS.md section 2).
#ifndef EPOCANVA_ENGLISH_TRANSLATOR_H_
#define EPOCANVA_ENGLISH_TRANSLATOR_H_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <rime/segmentation.h>
#include <rime/translation.h>
#include <rime/translator.h>

#include "english_diagnostics.h"
#include "english_lexicon.h"
#include "prefix_completer.h"

namespace epocanva {
namespace english {

// Outcome of trying to make a usable index out of one resolved path. A failed
// record is cached too, so a broken word list costs one attempt rather than one
// attempt per keystroke.
struct LexiconRecord {
  epocanva::en::LexiconStatus status = epocanva::en::LexiconStatus::kOk;
  // Pointer into the core's static string table, never freed and never
  // reconstructed, so it stays valid even if the cache is cleared.
  const char* status_literal = "never-attempted";
  // 1-based line the parse stopped on; 0 unless the failure is line-attributable.
  std::size_t error_line = 0;
  std::size_t entry_count = 0;
  std::shared_ptr<const epocanva::en::EnglishLexicon> lexicon;  // null unless ok

  static std::shared_ptr<const LexiconRecord> FromSuccessfulLoad(
      const std::shared_ptr<const epocanva::en::EnglishLexicon>& loaded);
  static std::shared_ptr<const LexiconRecord> FromFailedLoad(
      const epocanva::en::LexiconLoadInfo& info);
};

// Process-wide, path-keyed, read-after-load cache of compiled indexes.
class EnglishLexiconCache {
 public:
  // Returns the record for `resolved_path`, loading it only if this path has
  // never been attempted. lexicon_load_attempts moves on a miss only.
  // The path travels as a path object, never as a narrow string, so a non-ASCII
  // directory in the front end's shared_data_dir cannot be reinterpreted through
  // the ANSI code page on the way back in.
  static std::shared_ptr<const LexiconRecord> Acquire(
      const std::filesystem::path& resolved_path);

  // Test/diagnostic support: drops every cached index. Not called by the
  // Translator; exposed so a future phase can reload a changed word list without
  // changing what a running session sees.
  static void Clear();
};

class EpoCanvaEnglishTranslator : public rime::Translator {
 public:
  explicit EpoCanvaEnglishTranslator(const rime::Ticket& ticket);
  ~EpoCanvaEnglishTranslator() override;

  EpoCanvaEnglishTranslator(const EpoCanvaEnglishTranslator&) = delete;
  EpoCanvaEnglishTranslator& operator=(const EpoCanvaEnglishTranslator&) = delete;

  // `input` is this segment's own slice of the composition, in UTF-8 code units
  // (third_party/librime/src/rime/engine.cc:203-212 derives it with
  // substr(segment.start, segment.end - segment.start)).
  rime::an<rime::Translation> Query(const std::string& input,
                                    const rime::Segment& segment) override;

 private:
  std::shared_ptr<const LexiconRecord> record_;
  // Completions placed after the always-present original candidate.
  std::size_t max_completions_ = EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT;
  // P2.6: one-edit spelling suggestions, OFF unless the schema asks for them. The
  // default matters more than the feature: every schema written before P2.6 keeps
  // producing exactly the menu it produced before, and the integration test for the
  // disabled state is a regression test rather than a hope.
  bool spelling_enabled_ = false;
  std::size_t max_suggestions_ = EPOCANVA_ENGLISH_MAX_SUGGESTIONS_DEFAULT;
};

// --- diagnostics storage, defined in english_translator.cc ---------------------
// Called by the module's initialize()/finalize() (english_module.cc).
void NoteModuleInitialized();
void NoteModuleFinalized();

// Implements the three function pointers of EpoCanvaEnglishApi.
Bool CopyEnglishCounters(EpoCanvaEnglishCounters* out);
const char* LastLoadStatusName();
const char* EnglishBuildId();

}  // namespace english
}  // namespace epocanva

#endif  // EPOCANVA_ENGLISH_TRANSLATOR_H_
