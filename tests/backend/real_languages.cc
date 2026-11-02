#include "epocanva_backend.h"
#include <windows.h>
#include <psapi.h>
#include "product_io.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace {
int failed=0;void Check(bool value,const char* name){if(!value){std::cerr<<"FAIL "<<name<<'\n';++failed;}}
template<class T>T Init(){T value{};value.struct_size_bytes=sizeof(T);value.abi_version=1;return value;}
epocanva_utf8_view View(const std::string& text){return {text.data(),text.size()};}
std::string Hash(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{}),hash;std::wstring why;Check(epocanva::product::Sha256(bytes.data(),bytes.size(),&hash,&why),"real resource SHA");return hash;}
}
int main(int argc,char** argv){
  if(argc!=2&&argc!=3)return 2;const auto root=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
  const std::string model_directory=argc==3?argv[2]:"build/backend-training";
  std::vector<std::string> model_paths,hashes;std::vector<epocanva_backend_resource> resources;
  const char* ids[]={"en","zh","fr","ja","ko"};
  for(auto id:ids){auto path=root/model_directory/id/"model.tsv";model_paths.push_back(path.u8string());hashes.push_back(Hash(path));}
  const auto aff=(root/"build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff").u8string();
  const auto dic=(root/"build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic").u8string();
  const auto ja=(root/"build/backend-japanese-data/lexicon.bin").u8string();
  const auto affhash=Hash(std::filesystem::u8path(aff)),dichash=Hash(std::filesystem::u8path(dic)),jahash=Hash(std::filesystem::u8path(ja));
  for(std::uint32_t ordinal=0;ordinal<5;++ordinal){auto resource=Init<epocanva_backend_resource>();resource.language=ordinal+1;
    resource.model_path=View(model_paths[ordinal]);resource.model_sha256=View(hashes[ordinal]);
    if(ordinal==2){resource.conversion_path=View(aff);resource.conversion_sha256=View(affhash);resource.auxiliary_path=View(dic);resource.auxiliary_sha256=View(dichash);}
    if(ordinal==3){resource.conversion_path=View(ja);resource.conversion_sha256=View(jahash);}
    resources.push_back(resource);
  }
  const auto data=root/"build/backend-chinese-ranked";
  const auto dll=(root/"build/librime-english/rime/install/lib/rime.dll").u8string();
  const auto shared=(data/"shared").u8string(),user=(data/"user").u8string(),logs=(data/"logs").u8string();
  const std::string version="1.17.0";
  auto rime=Init<epocanva_engine_options>();rime.rime_dll_path=View(dll);rime.shared_data_dir=View(shared);rime.user_data_dir=View(user);rime.log_dir=View(logs);rime.expected_rime_version=View(version);
  auto options=Init<epocanva_backend_options>();options.resources=resources.data();options.resource_count=5;options.rime_options=&rime;
  epocanva_backend_handle backend=0;auto opened=epocanva_backend_open(&options,&backend);
  if(opened!=EPOCANVA_OK)std::cerr<<"backend_open_status="<<opened<<'\n';
  Check(opened==EPOCANVA_OK,"real five-language resources open");if(!backend)return 1;
  const std::vector<std::string> input={"hel","nihao","francais","nihon","dkssud"};
  const std::vector<std::string> target={"hello",u8"你好",u8"français",u8"日本",u8"안녕"};
  const std::uint32_t modes[]={EPOCANVA_BACKEND_LATIN,EPOCANVA_BACKEND_PINYIN,EPOCANVA_BACKEND_LATIN,EPOCANVA_BACKEND_ROMAJI,EPOCANVA_BACKEND_TWO_SET};
  for(std::uint32_t ordinal=0;ordinal<5;++ordinal){auto s=Init<epocanva_backend_session_options>();s.language=ordinal+1;s.input_mode=modes[ordinal];s.context={100+ordinal,1,EPOCANVA_PRIVACY_NORMAL,0};
    epocanva_backend_session_handle session=0;Check(epocanva_backend_session_open(backend,&s,&session)==EPOCANVA_OK,"language session opens");if(!session)continue;
    auto request=Init<epocanva_backend_request>();request.context=s.context;
    Check(epocanva_backend_set_input(session,&request,View(input[ordinal]))==EPOCANVA_OK,"real language input accepted");
    auto q=Init<epocanva_backend_query>();q.context=s.context;q.query_kind=EPOCANVA_BACKEND_COMPLETION;q.max_candidates=64;q.max_continuation_words=1;
    Check(epocanva_backend_query_candidates(session,&q)==EPOCANVA_OK,"real candidates queried");
    auto snapshot=Init<epocanva_backend_snapshot>();auto status=epocanva_backend_read_snapshot(session,&request,&snapshot,nullptr,0);
    Check(status==EPOCANVA_BUFFER_TOO_SMALL||status==EPOCANVA_OK,"real snapshot sizing");
    std::uint64_t selected=0;
    for(std::uint32_t row=0;row<snapshot.candidate_count;++row){auto candidate=Init<epocanva_backend_candidate>();
      epocanva_backend_read_candidate(session,&request,snapshot.snapshot_token,row,&candidate,nullptr,0);
      std::string bytes(static_cast<std::size_t>(candidate.required_utf8_bytes),'\0');
      Check(epocanva_backend_read_candidate(session,&request,snapshot.snapshot_token,row,&candidate,bytes.data(),bytes.size())==EPOCANVA_OK,"real candidate copy");
      if(bytes==target[ordinal])selected=candidate.candidate_token;
    }
    if(!selected)std::cerr<<"language="<<ids[ordinal]<<" candidate_count="<<snapshot.candidate_count<<'\n';
    Check(selected!=0,"real lexicon conversion includes expected candidate");
    if(selected){Check(epocanva_backend_select(session,&request,snapshot.snapshot_token,selected)==EPOCANVA_OK,"real candidate selection");
      auto commit=Init<epocanva_backend_commit>();Check(epocanva_backend_read_commit(session,&request,&commit,nullptr,0)==EPOCANVA_BUFFER_TOO_SMALL,"real commit sizing");
      std::string bytes(static_cast<std::size_t>(commit.required_utf8_bytes),'\0');Check(epocanva_backend_read_commit(session,&request,&commit,bytes.data(),bytes.size())==EPOCANVA_OK&&bytes==target[ordinal],"real commit payload");
      auto ack=Init<epocanva_backend_ack_result>();Check(epocanva_backend_ack_commit(session,&request,commit.commit_token,1,&ack)==EPOCANVA_OK&&!ack.learned,"consent-off never learns");
      q.query_kind=EPOCANVA_BACKEND_NEXT_WORD;q.max_candidates=5;Check(epocanva_backend_query_candidates(session,&q)==EPOCANVA_OK,"real per-language prediction");}
    request.context.privacy_class=EPOCANVA_PRIVACY_SENSITIVE;
    Check(epocanva_backend_set_input(session,&request,View(input[ordinal]))==EPOCANVA_PRIVACY_DENIED,"sensitive context rejected");
    Check(epocanva_backend_session_close(session)==EPOCANVA_OK,"language cleanup");
  }
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb=sizeof(memory);
  if(::GetProcessMemoryInfo(::GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
    std::cout<<"whole_backend_peak_working_set_bytes="<<memory.PeakWorkingSetSize<<'\n';
  else Check(false,"process memory measurement");
  Check(epocanva_backend_close(backend)==EPOCANVA_OK,"real backend closes");
  if(failed)return 1;std::cout<<"PASS actual five-language headless API\n";return 0;
}
