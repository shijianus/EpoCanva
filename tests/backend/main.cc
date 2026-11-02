#include "epocanva_backend.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failed=0;
void Check(bool good,const char* label){if(!good){std::cerr<<"FAIL "<<label<<'\n';++failed;}}
template<class T>T Init(){T value{};value.struct_size_bytes=sizeof(T);value.abi_version=1;return value;}
epocanva_utf8_view View(const std::string& text){return {text.data(),text.size()};}
struct Menu {epocanva_backend_snapshot snapshot;std::vector<epocanva_backend_candidate> rows;std::vector<std::string> text;};
Menu Query(epocanva_backend_session_handle session,const epocanva_backend_request& access,std::uint32_t kind=EPOCANVA_BACKEND_COMPLETION){
  auto query=Init<epocanva_backend_query>();query.context=access.context;query.learning_enabled=access.learning_enabled;
  query.query_kind=kind;query.max_candidates=16;query.max_continuation_words=1;
  Check(epocanva_backend_query_candidates(session,&query)==EPOCANVA_OK,"query succeeds");
  Menu menu;menu.snapshot=Init<epocanva_backend_snapshot>();
  auto status=epocanva_backend_read_snapshot(session,&access,&menu.snapshot,nullptr,0);
  Check(status==EPOCANVA_BUFFER_TOO_SMALL || status==EPOCANVA_OK,"snapshot sizing");
  std::string bytes(static_cast<std::size_t>(menu.snapshot.required_utf8_bytes),'\0');
  Check(epocanva_backend_read_snapshot(session,&access,&menu.snapshot,bytes.data(),bytes.size())==EPOCANVA_OK,"snapshot copy");
  for(std::uint32_t ordinal=0;ordinal<menu.snapshot.candidate_count;++ordinal){
    auto row=Init<epocanva_backend_candidate>();
    Check(epocanva_backend_read_candidate(session,&access,menu.snapshot.snapshot_token,ordinal,&row,nullptr,0)==EPOCANVA_BUFFER_TOO_SMALL,"candidate sizing");
    std::string text(static_cast<std::size_t>(row.required_utf8_bytes),'\0');
    Check(epocanva_backend_read_candidate(session,&access,menu.snapshot.snapshot_token,ordinal,&row,text.data(),text.size())==EPOCANVA_OK,"candidate copy");
    menu.text.push_back(text);menu.rows.push_back(row);
  }
  return menu;
}
}
int main(int argc,char** argv){
  if(argc==2 && std::string(argv[1])=="--force-failure"){Check(false,"intentional failure");return 1;}
  if(argc!=3 && argc!=5)return 2;
  try{
    const auto path=std::filesystem::absolute(std::filesystem::u8path(argv[1])).u8string();
    std::ifstream hash_file(std::filesystem::u8path(argv[2]));std::string digest;hash_file>>digest;
    std::vector<epocanva_backend_resource> resources;
    for(auto language:{EPOCANVA_BACKEND_EN,EPOCANVA_BACKEND_FR,EPOCANVA_BACKEND_JA,EPOCANVA_BACKEND_KO}){
      auto resource=Init<epocanva_backend_resource>();resource.language=language;resource.model_path=View(path);resource.model_sha256=View(digest);resources.push_back(resource);
    }
    auto options=Init<epocanva_backend_options>();options.resources=resources.data();options.resource_count=static_cast<std::uint32_t>(resources.size());
    std::string rime_path,shared,user,log,version="1.17.0",pinyin,zhuyin;
    std::vector<epocanva_utf8_view> schemas;
    auto rime=Init<epocanva_engine_options>();
    if(argc==5){
      rime_path=std::filesystem::absolute(std::filesystem::u8path(argv[3])).u8string();
      const auto root=std::filesystem::absolute(std::filesystem::u8path(argv[4]));
      shared=(root/"shared").u8string();user=(root/"user").u8string();log=(root/"logs").u8string();
      pinyin=(root/"shared/epocanva_backend_pinyin.schema.yaml").u8string();zhuyin=(root/"shared/epocanva_backend_zhuyin.schema.yaml").u8string();
      schemas={View(pinyin),View(zhuyin)};rime.rime_dll_path=View(rime_path);rime.shared_data_dir=View(shared);
      rime.user_data_dir=View(user);rime.log_dir=View(log);rime.expected_rime_version=View(version);
      rime.deployment_schema_paths=schemas.data();rime.deployment_schema_count=2;
      auto resource=Init<epocanva_backend_resource>();resource.language=EPOCANVA_BACKEND_ZH;resource.model_path=View(path);resource.model_sha256=View(digest);resources.push_back(resource);
      options.resources=resources.data();options.resource_count=static_cast<std::uint32_t>(resources.size());options.rime_options=&rime;
    }
    epocanva_backend_handle backend=0;
    Check(epocanva_backend_open(&options,&backend)==EPOCANVA_BACKEND_INVALID_RESOURCE,"production denies fixture model");
    options.allow_test_fixture=1;
    const std::string prohibited="\\\\127.0.0.1\\not-a-share\\data.bin";
    resources[0].conversion_path=View(prohibited);
    Check(epocanva_backend_open(&options,&backend)==EPOCANVA_INVALID_ARGUMENT,"irrelevant network conversion path refused before open");
    resources[0].conversion_path={};
    Check(epocanva_backend_open(&options,&backend)==EPOCANVA_OK && backend,"real model-backed engine opens");
    if(!backend)return 1;
    auto session_options=Init<epocanva_backend_session_options>();session_options.context={23,1,EPOCANVA_PRIVACY_NORMAL,0};
    session_options.language=EPOCANVA_BACKEND_EN;session_options.input_mode=EPOCANVA_BACKEND_LATIN;session_options.learning_enabled=1;
    epocanva_backend_session_handle session=0;
    Check(epocanva_backend_session_open(backend,&session_options,&session)==EPOCANVA_OK,"English session opens");
    auto access=Init<epocanva_backend_request>();access.context=session_options.context;access.learning_enabled=1;
    const std::string input="good";
    Check(epocanva_backend_set_input(session,&access,View(input))==EPOCANVA_OK,"input set");
    auto menu=Query(session,access);std::size_t chosen=menu.rows.size();
    for(std::size_t ordinal=0;ordinal<menu.rows.size();++ordinal)if(menu.text[ordinal]=="good" && menu.rows[ordinal].lexeme_count)chosen=ordinal;
    Check(chosen<menu.rows.size(),"admitted exact word separate from raw");
    Check(!menu.rows.empty() && menu.rows[0].kind==EPOCANVA_BACKEND_RAW && menu.rows[0].lexeme_count==0,"raw carries no learnable identity");
    if(chosen<menu.rows.size()){
      Check(epocanva_backend_select(session,&access,menu.snapshot.snapshot_token,menu.rows[chosen].candidate_token)==EPOCANVA_OK,"candidate selection");
      auto commit=Init<epocanva_backend_commit>();
      Check(epocanva_backend_read_commit(session,&access,&commit,nullptr,0)==EPOCANVA_BUFFER_TOO_SMALL,"commit sizing retained");
      auto acknowledgement=Init<epocanva_backend_ack_result>();
      Check(epocanva_backend_ack_commit(session,&access,commit.commit_token,1,&acknowledgement)==EPOCANVA_BUSY,"sizing cannot acknowledge");
      std::string output(static_cast<std::size_t>(commit.required_utf8_bytes),'\0');
      Check(epocanva_backend_read_commit(session,&access,&commit,output.data(),output.size())==EPOCANVA_OK && output=="good","commit bytes");
      Check(epocanva_backend_ack_commit(session,&access,commit.commit_token,1,&acknowledgement)==EPOCANVA_OK && acknowledgement.consumed && acknowledgement.learned,"confirmed commit learns once");
      Check(epocanva_backend_ack_commit(session,&access,commit.commit_token,1,&acknowledgement)==EPOCANVA_STALE_SNAPSHOT,"duplicate ack rejected");
      auto next=Query(session,access,EPOCANVA_BACKEND_NEXT_WORD);
      Check(!next.text.empty() && next.text.front()=="morning","confirmed context predicts next word");
    }
    auto denied=access;denied.context.privacy_class=EPOCANVA_PRIVACY_SENSITIVE;
    Check(epocanva_backend_set_input(session,&denied,View(input))==EPOCANVA_PRIVACY_DENIED,"sensitive request denied");
    Check(epocanva_backend_cancel(session,&denied)==EPOCANVA_OK,"sensitive can discard");
    Check(epocanva_backend_close(backend)==EPOCANVA_BUSY,"live session prevents shutdown");
    Check(epocanva_backend_session_close(session)==EPOCANVA_OK,"session close");
    for(auto language:{EPOCANVA_BACKEND_FR,EPOCANVA_BACKEND_JA,EPOCANVA_BACKEND_KO}){
      session_options.language=language;session_options.input_mode=language==EPOCANVA_BACKEND_JA?EPOCANVA_BACKEND_ROMAJI:
          language==EPOCANVA_BACKEND_KO?EPOCANVA_BACKEND_TWO_SET:EPOCANVA_BACKEND_LATIN;
      session_options.learning_enabled=0;access.learning_enabled=0;
      Check(epocanva_backend_session_open(backend,&session_options,&session)==EPOCANVA_OK,"other language session opens");
      std::string text=language==EPOCANVA_BACKEND_FR?"caf":language==EPOCANVA_BACKEND_JA?"konnichiha":"dkssud";
      Check(epocanva_backend_set_input(session,&access,View(text))==EPOCANVA_OK,"language real compose input");
      menu=Query(session,access);Check(!menu.rows.empty(),"real language candidate result");
      Check(epocanva_backend_session_close(session)==EPOCANVA_OK,"language session cleanup");
    }
    if(argc==5){
      for(auto mode:{EPOCANVA_BACKEND_PINYIN,EPOCANVA_BACKEND_ZHUYIN}){
        session_options.language=EPOCANVA_BACKEND_ZH;session_options.input_mode=mode;
        Check(epocanva_backend_session_open(backend,&session_options,&session)==EPOCANVA_OK,"Chinese real schema opens");
        const std::string reading=mode==EPOCANVA_BACKEND_PINYIN?"nihao":"su3cl3";
        Check(epocanva_backend_set_input(session,&access,View(reading))==EPOCANVA_OK,"Chinese real composition");
        menu=Query(session,access);chosen=menu.rows.size();
        for(std::size_t ordinal=0;ordinal<menu.rows.size();++ordinal)if(menu.text[ordinal]==u8"你好")chosen=ordinal;
        if(chosen==menu.rows.size()){
          std::cerr<<"Chinese fixture mode="<<mode<<" candidate_count="<<menu.rows.size()<<'\n';
          for(const auto& value:menu.text)std::cerr<<"fixture candidate="<<value<<'\n';
        }
        Check(chosen<menu.rows.size(),"actual pinned dictionary yields Hanzi");
        if(chosen<menu.rows.size()){
          Check(epocanva_backend_select(session,&access,menu.snapshot.snapshot_token,menu.rows[chosen].candidate_token)==EPOCANVA_OK,"Chinese actual engine selects candidate");
          auto commit=Init<epocanva_backend_commit>();
          Check(epocanva_backend_read_commit(session,&access,&commit,nullptr,0)==EPOCANVA_BUFFER_TOO_SMALL,"Chinese commit sizing");
          std::string text(static_cast<std::size_t>(commit.required_utf8_bytes),'\0');
          Check(epocanva_backend_read_commit(session,&access,&commit,text.data(),text.size())==EPOCANVA_OK&&text==u8"你好","actual Rime commit text");
          auto ack=Init<epocanva_backend_ack_result>();Check(epocanva_backend_ack_commit(session,&access,commit.commit_token,1,&ack)==EPOCANVA_OK&&!ack.learned,"text-only Rime identity not learned");
        }
        Check(epocanva_backend_session_close(session)==EPOCANVA_OK,"Chinese session closes");
      }
    }
    Check(epocanva_backend_close(backend)==EPOCANVA_OK,"engine close");
  }catch(...){std::cerr<<"FAIL unexpected exception\n";return 1;}
  if(failed)return 1;std::cout<<"PASS backend contracts\n";return 0;
}
