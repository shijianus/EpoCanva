#include "japanese_decoder.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include "engine/backend/local_resource.h"
#include "engine/prediction/model.h"
#include "product_io.h"

namespace epocanva::japanese_decoder {
namespace {
std::uint32_t Read32(const std::vector<unsigned char>& bytes,std::size_t* at){
  if(*at>bytes.size()||bytes.size()-*at<4)throw std::runtime_error("truncated");
  std::uint32_t result=0;for(unsigned shift=0;shift<32;shift+=8)result|=static_cast<std::uint32_t>(bytes[(*at)++])<<shift;return result;
}
std::uint16_t Read16(const std::vector<unsigned char>& bytes,std::size_t* at){
  if(*at>bytes.size()||bytes.size()-*at<2)throw std::runtime_error("truncated");
  auto value=static_cast<std::uint16_t>(bytes[*at]|static_cast<unsigned>(bytes[*at+1])<<8);*at+=2;return value;
}
bool Kana(std::string_view text,std::vector<std::size_t>* boundaries){
  if(text.empty()||text.size()>512||!prediction::IsValidUtf8(text))return false;
  boundaries->push_back(0);
  for(std::size_t at=0;at<text.size();){
    auto byte=static_cast<unsigned char>(text[at++]);std::uint32_t scalar=byte;unsigned continuation=0;
    if(byte>=0xf0){scalar&=7;continuation=3;}else if(byte>=0xe0){scalar&=15;continuation=2;}else if(byte>=0xc0){scalar&=31;continuation=1;}
    while(continuation--)scalar=(scalar<<6)|(static_cast<unsigned char>(text[at++])&63);
    if(!((scalar>=0x3041&&scalar<=0x309f)||(scalar>=0x30a1&&scalar<=0x30ff)||scalar==0x3005))return false;
    boundaries->push_back(at);
  }
  return true;
}
}
struct Decoder::Impl {
  struct Entry {std::string reading,surface;std::uint32_t left=0,right=0;std::int32_t cost=0;};
  std::uint32_t dimension=0;
  std::vector<std::int16_t> connections;
  std::vector<Entry> entries;
  std::size_t bytes=0;
  std::int16_t Cost(std::uint32_t right,std::uint32_t left)const{return connections[static_cast<std::size_t>(right)*dimension+left];}
};
Decoder::Decoder()=default;
Decoder::~Decoder()=default;
Decoder::Decoder(Decoder&&)noexcept=default;
Decoder& Decoder::operator=(Decoder&&)noexcept=default;
bool Decoder::available()const noexcept{return impl_!=nullptr;}
std::size_t Decoder::resident_bytes()const noexcept{return impl_?impl_->bytes:0;}
std::uint32_t Decoder::connection_dimension()const noexcept{return impl_?impl_->dimension:0;}
Status Decoder::Load(const std::string& path_utf8,const LoadOptions& options){
  impl_.reset();
  try{
    if(options.expected_sha256.size()!=64)return Status::kInvalidArgument;
    backend::LocalResource held;
    if(!held.Open(std::filesystem::u8path(path_utf8)))return Status::kPathRejected;
    std::error_code file_error;
    const auto file_bytes=std::filesystem::file_size(held.path(),file_error);
    if(file_error)return Status::kFileReadFailed;
    if(file_bytes>options.max_source_bytes)return Status::kResourceTooLarge;
    std::ifstream stream(held.path(),std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(file_bytes));
    if(!stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))return Status::kFileReadFailed;
    std::string digest;std::wstring why;
    if(!product::Sha256(bytes.data(),bytes.size(),&digest,&why))return Status::kFileReadFailed;
    if(digest!=options.expected_sha256)return Status::kResourceHashMismatch;
    if(bytes.size()<16||std::memcmp(bytes.data(),"EPCJA01\0",8))return Status::kHeaderInvalid;
    std::size_t at=8;
    auto model=std::make_unique<Impl>();model->dimension=Read32(bytes,&at);auto count=Read32(bytes,&at);
    if(!model->dimension||model->dimension>4096||!count||count>2000000)return Status::kInvalidResource;
    const auto cells=static_cast<std::size_t>(model->dimension)*model->dimension;
    if(cells>bytes.size()/2)return Status::kInvalidResource;
    model->connections.reserve(cells);
    for(std::size_t ordinal=0;ordinal<cells;++ordinal)model->connections.push_back(static_cast<std::int16_t>(Read16(bytes,&at)));
    model->entries.reserve(count);model->bytes=cells*2+sizeof(Impl)+static_cast<std::size_t>(count)*sizeof(Impl::Entry);
    for(std::uint32_t ordinal=0;ordinal<count;++ordinal){
      const auto reading_bytes=Read16(bytes,&at),surface_bytes=Read16(bytes,&at);
      Impl::Entry entry;entry.left=Read32(bytes,&at);entry.right=Read32(bytes,&at);entry.cost=static_cast<std::int32_t>(Read32(bytes,&at));
      if(!reading_bytes||!surface_bytes||reading_bytes>1024||surface_bytes>1024||entry.left>=model->dimension||entry.right>=model->dimension||
          at>bytes.size()||static_cast<std::size_t>(reading_bytes)+surface_bytes>bytes.size()-at)return Status::kInvalidResource;
      entry.reading.assign(reinterpret_cast<const char*>(bytes.data()+at),reading_bytes);at+=reading_bytes;
      entry.surface.assign(reinterpret_cast<const char*>(bytes.data()+at),surface_bytes);at+=surface_bytes;
      if(!prediction::IsValidUtf8(entry.reading)||!prediction::IsValidUtf8(entry.surface)||entry.reading.find('\0')!=entry.reading.npos||entry.surface.find('\0')!=entry.surface.npos)
        return Status::kInvalidResource;
      model->bytes+=reading_bytes+surface_bytes+2;
      if(model->bytes>options.max_memory_bytes)return Status::kResourceTooLarge;
      model->entries.push_back(std::move(entry));
    }
    if(at!=bytes.size())return Status::kInvalidResource;
    std::sort(model->entries.begin(),model->entries.end(),[](const auto& left,const auto& right){
      if(left.reading!=right.reading)return left.reading<right.reading;
      if(left.cost!=right.cost)return left.cost<right.cost;
      return left.surface<right.surface;
    });
    impl_=std::move(model);return Status::kOk;
  }catch(...){return Status::kInvalidResource;}
}
Status Decoder::Convert(std::string_view input,std::size_t maximum,std::vector<Candidate>* output)const{
  if(output)output->clear();
  if(!output||!maximum||maximum>64)return Status::kInvalidArgument;
  if(!impl_)return Status::kUnavailable;
  std::vector<std::size_t> boundaries;
  if(!Kana(input,&boundaries))return prediction::IsValidUtf8(input)?Status::kInvalidArgument:Status::kInvalidUtf8;
  struct Path {std::int64_t cost=0;std::uint32_t right=0;std::vector<Segment> segments;std::string text;bool known=true;};
  const auto compare=[](const Path& a,const Path& b){return a.cost!=b.cost?a.cost<b.cost:a.text<b.text;};
  std::vector<std::map<std::uint32_t,std::vector<Path>>> dp(input.size()+1);
  dp[0][0].push_back({});
  std::size_t transitions=0;
  constexpr std::size_t limit=200000;
  bool exhausted=false;
  for(std::size_t boundary_ordinal=0;boundary_ordinal+1<boundaries.size()&&!exhausted;++boundary_ordinal){
    const auto begin=boundaries[boundary_ordinal];if(dp[begin].empty())continue;
    bool found=false;
    auto append=[&](const Impl::Entry& entry,std::size_t end,bool known){
      for(const auto& state:dp[begin])for(const auto& previous:state.second){
        if(++transitions>limit){exhausted=true;return;}
        auto path=previous;path.cost+=impl_->Cost(previous.right,entry.left)+entry.cost;path.right=entry.right;
        path.text+=entry.surface;path.known=path.known&&known;
        path.segments.push_back({entry.surface,entry.reading,begin,end,entry.left,entry.right,!known});
        auto& candidates=dp[end][entry.right];
        auto same=std::find_if(candidates.begin(),candidates.end(),[&](const Path& other){return other.text==path.text;});
        if(same!=candidates.end()){if(path.cost<same->cost)*same=std::move(path);}else candidates.push_back(std::move(path));
        std::sort(candidates.begin(),candidates.end(),compare);if(candidates.size()>maximum)candidates.resize(maximum);
      }
    };
    for(std::size_t end_ordinal=boundary_ordinal+1;end_ordinal<boundaries.size()&&!exhausted;++end_ordinal){
      const auto end=boundaries[end_ordinal];const auto reading=input.substr(begin,end-begin);
      auto entry=std::lower_bound(impl_->entries.begin(),impl_->entries.end(),reading,
          [](const Impl::Entry& row,std::string_view text){return row.reading<text;});
      for(;entry!=impl_->entries.end()&&entry->reading==reading&&!exhausted;++entry){found=true;append(*entry,end,true);}
    }
    if(!found&&!exhausted){const auto end=boundaries[boundary_ordinal+1];Impl::Entry unknown;
      unknown.reading=std::string(input.substr(begin,end-begin));unknown.surface=unknown.reading;unknown.cost=16000;append(unknown,end,false);}
  }
  std::vector<Path> final;
  for(auto& state:dp.back())for(auto& path:state.second){path.cost+=impl_->Cost(path.right,0);final.push_back(std::move(path));}
  std::sort(final.begin(),final.end(),compare);std::set<std::string> seen;
  for(auto& path:final){if(!seen.insert(path.text).second)continue;
    output->push_back({std::move(path.text),std::move(path.segments),path.cost,path.known&&!exhausted});
    if(output->size()==maximum)break;}
  return exhausted?Status::kSearchBudgetExceeded:Status::kOk;
}
const char* StatusName(Status status)noexcept{
  switch(status){case Status::kOk:return "ok";case Status::kUnavailable:return "unavailable";case Status::kInvalidArgument:return "invalid-argument";
    case Status::kInvalidUtf8:return "invalid-utf8";case Status::kInvalidResource:return "invalid-resource";case Status::kResourceHashMismatch:return "resource-hash-mismatch";
    case Status::kResourceTooLarge:return "resource-too-large";case Status::kSearchBudgetExceeded:return "search-budget-exceeded";}
  if(status==Status::kPathRejected)return "path-rejected";
  if(status==Status::kFileReadFailed)return "file-read-failed";
  if(status==Status::kHeaderInvalid)return "header-invalid";
  if(status==Status::kRecordInvalid)return "record-invalid";
  return "unknown-status";
}
}
