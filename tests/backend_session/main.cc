#include "session.h"
#include <iostream>
#include <string>
#include <thread>

namespace {
namespace flow=epocanva::backend_session;
namespace learn=epocanva::learning;
int failed=0;
void Check(bool value,const char* label){if(!value){++failed;std::cerr<<"FAIL "<<label<<'\n';}}
learn::Access Allowed(){return {learn::PrivacyClass::kNormal,true,10,20};}
std::unique_ptr<flow::Session> Make(const std::string& ns="en.test"){
  return flow::Session::Create({ns,std::string(64,'a'),{1,2,3}},std::string(64,'b'),Allowed());
}
flow::Snapshot Publish(flow::Session& session,std::vector<learn::LexemeId> ids={1}){
  auto access=Allowed();Check(session.SetInput("a","a",access)==flow::Status::kOk,"set input");
  flow::Candidate row;row.text_utf8="alpha";row.lexeme_ids=std::move(ids);row.input_end_utf8_byte=1;
  Check(session.Publish({row},true,access)==flow::Status::kOk,"publish");
  flow::Snapshot snapshot;Check(session.ReadSnapshot(access,&snapshot)==flow::Status::kOk,"read snapshot");return snapshot;
}
flow::Commit Select(flow::Session& session,std::vector<learn::LexemeId> ids={1}){
  auto snapshot=Publish(session,std::move(ids));
  Check(session.Select(snapshot.token,snapshot.candidates[0].token,"",Allowed())==flow::Status::kOk,"select");
  flow::Commit commit;Check(session.ReadCommit(Allowed(),&commit)==flow::Status::kOk,"read commit");return commit;
}
void Protocol(){
  auto session=Make();auto commit=Select(*session);learn::Snapshot learned;
  session->ReadLearning(Allowed(),&learned);Check(learned.words.empty(),"selection does not learn");
  auto ack=session->Acknowledge(commit.token,true,[]{return Allowed();});
  Check(ack.status==flow::Status::kBusy&&!ack.consumed,"copy sizing is not confirmation");
  Check(session->MarkCommitCopied(commit.token,Allowed())==flow::Status::kOk,"mark full copied");
  ack=session->Acknowledge(commit.token,true,[]{return Allowed();});
  Check(ack.status==flow::Status::kOk&&ack.learned&&ack.consumed&&ack.context_advanced,"applied confirmation learns");
  session->ReadLearning(Allowed(),&learned);Check(learned.revision==1&&learned.words.size()==1&&learned.words[0].count==1,"one count");
  Check(session->Acknowledge(commit.token,true,[]{return Allowed();}).status==flow::Status::kStale,"duplicate confirmation stale");
  session->ReadLearning(Allowed(),&learned);Check(learned.revision==1,"duplicate no increment");
}
void Context(){
  auto session=Make();auto commit=Select(*session,{1,2});session->MarkCommitCopied(commit.token,Allowed());
  auto off=Allowed();off.learning_enabled=false;
  auto result=session->Acknowledge(commit.token,true,[off]{return off;});
  Check(result.context_advanced&&!result.learned&&session->Context(off)==std::vector<learn::LexemeId>({1,2}),"static context survives learning off");
  commit=Select(*session,{3});session->MarkCommitCopied(commit.token,Allowed());
  learn::LexemeId recorded_previous=99;
  session->Acknowledge(commit.token,true,[]{return Allowed();},[&](const auto& selected,const auto&){recorded_previous=selected.previous_id;return learn::Status::kOk;});
  Check(recorded_previous==0,"learning-off context never enters persisted pair");
  commit=Select(*session,{});session->MarkCommitCopied(commit.token,Allowed());
  session->Acknowledge(commit.token,true,[]{return Allowed();});Check(session->Context(Allowed()).empty(),"raw unknown breaks context");
}
void Privacy(){
  auto session=Make();auto commit=Select(*session);session->MarkCommitCopied(commit.token,Allowed());
  auto denied=Allowed();denied.privacy=learn::PrivacyClass::kSensitive;
  std::size_t called=0;
  auto result=session->Acknowledge(commit.token,true,[denied]{return denied;},[&](const auto&,const auto&){++called;return learn::Status::kOk;});
  Check(result.status==flow::Status::kDenied&&result.consumed&&called==0,"sensitive acknowledgement zero persistence");
  Check(session->Context(Allowed()).empty(),"privacy clears context");
  commit=Select(*session);session->MarkCommitCopied(commit.token,Allowed());
  std::size_t checks=0;
  result=session->Acknowledge(commit.token,true,[&]{return ++checks==1?Allowed():denied;},[&](const auto&,const auto&){++called;return learn::Status::kOk;});
  Check(!result.learned&&called==0&&result.consumed,"permission flip before storage no persistence");
  commit=Select(*session);session->MarkCommitCopied(commit.token,Allowed());
  result=session->Acknowledge(commit.token,true,[]()->learn::Access{throw 42;});
  Check(result.status==flow::Status::kDenied,"throwing permission fails closed");
}
void Validation(){
  auto session=Make();auto access=Allowed();
  Check(session->SetInput("\xf0\x80\x80\x80","",access)==flow::Status::kInvalid,"overlong UTF8 rejected");
  Check(session->SetInput(u8"文😀",u8"文😀",access)==flow::Status::kOk,"valid scalar input");
  flow::Candidate row;row.text_utf8=u8"文😀";row.input_begin_utf8_byte=1;row.input_end_utf8_byte=7;
  Check(session->Publish({row},true,access)==flow::Status::kInvalid,"inside UTF8 byte rejected");
  row.input_begin_utf8_byte=0;row.lexeme_ids={0};Check(session->Publish({row},true,access)==flow::Status::kInvalid,"zero ID rejected");
  row.lexeme_ids={99};Check(session->Publish({row},true,access)==flow::Status::kInvalid,"unknown ID rejected");
  row.lexeme_ids={1};Check(session->Publish({row},true,access)==flow::Status::kOk,"valid byte ranges");
  flow::Snapshot snapshot;session->ReadSnapshot(access,&snapshot);
  Check(session->AcceptExternal(snapshot.token,snapshot.candidates[0].token,u8"文章","weiwan",u8"未完",access)==flow::Status::kOk,"commit with remaining preedit");
  flow::Commit commit;session->ReadCommit(access,&commit);
  Check(commit.text_utf8==u8"文章"&&commit.remaining_preedit_utf8==u8"未完"&&commit.lexeme_ids.empty(),"external difference clears provenance not remaining text");
  session->MarkCommitCopied(commit.token,access);session->Acknowledge(commit.token,true,[access]{return access;});
  session->ReadSnapshot(access,&snapshot);Check(snapshot.preedit_utf8==u8"未完"&&snapshot.raw_utf8=="weiwan","raw and display preedit remain distinct after ack");
}
void Catalogs(){
  auto left=Make();auto right=Make("fr.test");auto one=Publish(*left);auto two=Publish(*right);
  Check(one.token!=two.token&&one.candidates[0].token!=two.candidates[0].token,"tokens globally distinct");
  Check(right->Select(one.token,one.candidates[0].token,"",Allowed())==flow::Status::kStale,"foreign session refused");
  auto changed=Allowed();++changed.generation;Check(left->Select(one.token,one.candidates[0].token,"",changed)==flow::Status::kStale,"generation stale");
}
void Reset(){auto session=Make();auto commit=Select(*session);auto denied=Allowed();denied.privacy=learn::PrivacyClass::kUnknown;
  Check(session->Reset(denied)==flow::Status::kOk,"privacy can discard");flow::Commit out;Check(session->ReadCommit(Allowed(),&out)==flow::Status::kNoCommit,"discard pending commit");
  Check(session->MarkCommitCopied(commit.token,Allowed())==flow::Status::kStale,"discarded token stale");}
void Failure(){auto session=Make();auto commit=Select(*session);session->MarkCommitCopied(commit.token,Allowed());std::size_t calls=0;
  auto result=session->Acknowledge(commit.token,true,[]{return Allowed();},[&](const auto&,const auto&){++calls;return learn::Status::kIoError;});
  Check(result.consumed&&!result.learned&&result.status==flow::Status::kOk&&calls==1,"storage failure preserves confirmed host event");
  Check(session->Acknowledge(commit.token,true,[]{return Allowed();}).status==flow::Status::kStale,"uncertain storage not retried");}
void WrongThread(){auto session=Make();flow::Status status=flow::Status::kOk;std::thread t([&]{status=session->SetInput("a","a",Allowed());});t.join();Check(status==flow::Status::kWrongThread,"owner thread enforced");}
}
int main(int argc,char** argv){if(argc!=2)return 2;const std::string mode=argv[1];
  if(mode=="protocol")Protocol();else if(mode=="context")Context();else if(mode=="privacy")Privacy();else if(mode=="validation")Validation();
  else if(mode=="catalogs")Catalogs();else if(mode=="reset")Reset();else if(mode=="failure")Failure();else if(mode=="wrong_thread")WrongThread();
  else if(mode=="forcefail")Check(false,"deliberate negative");else return 2;
  if(failed)return 1;std::cout<<"RESULT PASS\n";return 0;}
