#include "japanese_decoder.h"
#include <iostream>
#include <string>
#include <vector>
namespace {int failed=0;void Check(bool value,const char* label){if(!value){std::cerr<<"FAIL "<<label<<'\n';++failed;}}}
int main(int argc,char** argv){
  if(argc==2&&std::string(argv[1])=="--forcefail"){Check(false,"intentional");return 1;}
  if(argc!=3)return 2;
  using namespace epocanva::japanese_decoder;
  Decoder decoder;LoadOptions options;options.expected_sha256=argv[2];
  const auto loaded=decoder.Load(argv[1],options);
  if(loaded!=Status::kOk)std::cerr<<"decoder_load_status="<<StatusName(loaded)<<'\n';
  Check(loaded==Status::kOk,"actual dictionary loads");
  if(!decoder.available())return 1;
  Check(decoder.connection_dimension()==2672,"actual POS matrix dimension");
  for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{u8"にほん",u8"日本"},{u8"わたし",u8"私"},{u8"がっこう",u8"学校"}}){
    std::vector<Candidate> out;Check(decoder.Convert(pair.first,16,&out)==Status::kOk,"native conversion query");
    bool found=false;for(const auto& candidate:out)if(candidate.text_utf8==pair.second)found=true;
    if(!found){std::cerr<<"fixture result count="<<out.size()<<'\n';for(const auto& row:out)std::cerr<<"fixture="<<row.text_utf8<<'\n';}
    Check(found,"actual Kana to Kanji expected candidate");
  }
  std::vector<Candidate> phrase;
  Check(decoder.Convert(u8"わたしはがっこう",16,&phrase)==Status::kOk&&!phrase.empty(),"actual multiword Viterbi path");
  bool segmented=false;for(const auto& row:phrase){if(row.segments.size()>1)segmented=true;std::size_t prior=0;for(const auto& seg:row.segments){Check(seg.input_begin_utf8_bytes==prior&&seg.input_end_utf8_bytes>prior,"segments preserve original UTF8 offsets");prior=seg.input_end_utf8_bytes;}Check(prior==std::string(u8"わたしはがっこう").size(),"path consumes entire reading");}
  Check(segmented,"decoder composes multiple lexical entries");
  Check(decoder.Convert("http://x",8,&phrase)==Status::kInvalidArgument,"nonkana protected");
  Check(decoder.Convert("\xff",8,&phrase)==Status::kInvalidUtf8,"malformed UTF8");
  Decoder wrong;options.expected_sha256=std::string(64,'0');Check(wrong.Load(argv[1],options)==Status::kResourceHashMismatch&&!wrong.available(),"wrong hash refused");
  std::cout<<"decoder_resident_accounted_bytes="<<decoder.resident_bytes()<<'\n';
  if(failed)return 1;std::cout<<"PASS Japanese real decoder\n";return 0;
}
