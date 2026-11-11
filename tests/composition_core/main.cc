#include "test_support.h"

#include "romaji_composer.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "ARGUMENT ERROR expected_one_suite_argument\n";
    return 2;
  }
  const std::string_view argument = argv[1];
  composition_test::TestContext test;
  if (argument == "--japanese") {
    composition_test::RunJapaneseTests(test);
    return test.Finish("japanese");
  }
  if (argument == "--korean") {
    composition_test::RunKoreanTests(test);
    return test.Finish("korean");
  }
  if (argument == "--invariants") {
    composition_test::RunInvariantTests(test);
    return test.Finish("invariants");
  }
  if (argument == "--force-fail") {
    epocanva::ja::RomajiComposer composer;
    composition_test::Feed(test, composer, "ka", "intentional.acceptance");
    test.Check(composer.CommitUtf8() == u8"き", "intentional.wrong_expected_kana");
    return test.Finish("intentional_control");
  }
  std::cerr << "ARGUMENT ERROR unknown_suite_argument\n";
  return 2;
}
