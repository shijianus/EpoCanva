#include "epocanva_backend.h"
#include "engine/prediction/model.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace pred = epocanva::prediction;
std::string Json(std::string_view text) {
  std::string value = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char byte : text) {
    if (byte == '"' || byte == '\\') { value.push_back('\\'); value.push_back(static_cast<char>(byte)); }
    else if (byte < 0x20) { value += "\\u00"; value.push_back(hex[byte >> 4]); value.push_back(hex[byte & 15]); }
    else value.push_back(static_cast<char>(byte));
  }
  return value + '"';
}
std::vector<std::string> Fields(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin_utf8_byte=0;
  for (;;) {
    const auto separator_utf8_byte=line.find('\t',begin_utf8_byte);
    fields.push_back(line.substr(begin_utf8_byte,separator_utf8_byte==line.npos?line.npos:separator_utf8_byte-begin_utf8_byte));
    if(separator_utf8_byte==line.npos)return fields;
    begin_utf8_byte=separator_utf8_byte+1;
  }
}
int Evaluate(const std::filesystem::path& model_path,const std::filesystem::path& requests_path,
             const std::filesystem::path& output_path,bool baseline,bool test_fixture) {
  pred::LoadOptions load;load.allow_test_fixture=test_fixture;
  pred::BuildInfo info;
  auto model=pred::Model::Load(model_path,load,&info);
  if(!model){std::cerr<<"model-load-failed status="<<pred::StatusName(info.status)<<'\n';return 3;}
  std::ifstream requests(requests_path,std::ios::binary);
  if(!requests){std::cerr<<"requests-unavailable\n";return 3;}
  if(std::filesystem::exists(output_path)){std::cerr<<"output-exists\n";return 3;}
  std::ofstream output(output_path,std::ios::binary);
  if(!output){std::cerr<<"output-unavailable\n";return 3;}
  std::string line;
  std::size_t cases=0;
  while(std::getline(requests,line)) {
    if(!line.empty() && line.back()=='\r')line.pop_back();
    const auto fields=Fields(line);
    if(fields.size()!=5 || fields[0].empty() || !pred::IsValidLanguageId(fields[1])) {
      std::cerr<<"invalid-request-record\n";return 4;
    }
    pred::Context context;
    if(!baseline)for(std::size_t ordinal=2;ordinal<=3;++ordinal) {
      if(fields[ordinal].empty())continue;
      std::string canonical;
      if(!pred::NormalizeLexemeUtf8(fields[ordinal],&canonical)){context.clear();continue;}
      const auto* lexeme=model->Find(fields[1],canonical);
      if(!lexeme)context.clear();else context.push_back(lexeme->id);
    }
    pred::PredictOptions options;options.language_id=fields[1];options.max_suggestions=5;options.max_words=1;
    const auto start=std::chrono::steady_clock::now();
    const auto result=model->Predict(fields[4].empty()?std::optional<std::string_view>{}:std::optional<std::string_view>{fields[4]},context,options);
    const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    output<<"{\"id\":"<<Json(fields[0])<<",\"language\":"<<Json(fields[1])<<",\"ranked\":[";
    bool first=true;
    for(const auto& suggestion:result.suggestions){if(suggestion.tokens_utf8.size()!=1)continue;if(!first)output<<',';first=false;output<<Json(suggestion.tokens_utf8.front());}
    output<<"],\"latency_ms\":"<<milliseconds<<",\"status\":"<<Json(pred::StatusName(result.status))
          <<",\"complete\":"<<(result.complete?"true":"false")<<",\"counters\":{\"scored\":"<<result.candidates_scored
          <<",\"index_entries\":"<<result.index_entries_visited<<"}}\n";
    if(!output){std::cerr<<"output-write-failed\n";return 5;}++cases;
  }
  if(!requests.eof() || !cases){std::cerr<<"requests-empty-or-unreadable\n";return 4;}
  std::cout<<"evaluated_cases="<<cases<<'\n';return 0;
}
}
int main(int argc,char** argv) {
  try {
    std::filesystem::path model,requests,output;
    bool baseline=false,test_fixture=false;
    for(int ordinal=1;ordinal<argc;++ordinal){const std::string argument=argv[ordinal];
      if(argument=="--version"){std::cout<<"EpoCanva headless backend experimental ABI 1\n";return 0;}
      if(argument=="--baseline-unigram")baseline=true;
      else if(argument=="--allow-test-fixture")test_fixture=true;
      else if((argument=="--model"||argument=="--evaluate"||argument=="--output")&&ordinal+1<argc){
        const auto path=std::filesystem::u8path(argv[++ordinal]);if(argument=="--model")model=path;else if(argument=="--evaluate")requests=path;else output=path;
      }else {std::cerr<<"invalid-arguments\n";return 2;}
    }
    if(model.empty()||requests.empty()||output.empty()){std::cerr<<"usage: --model local-model.tsv --evaluate requests.tsv --output results.jsonl [--baseline-unigram]\n";return 2;}
    return Evaluate(model,requests,output,baseline,test_fixture);
  }catch(...){std::cerr<<"backend-cli-failed\n";return 10;}
}
