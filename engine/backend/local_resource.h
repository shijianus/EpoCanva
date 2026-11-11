#ifndef EPOCANVA_BACKEND_LOCAL_RESOURCE_H_
#define EPOCANVA_BACKEND_LOCAL_RESOURCE_H_
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace epocanva::backend {
// Pins every path component and the data file while a loader opens it. Rejects
// network mappings and name-surrogate reparses before following any child.
class LocalResource {
 public:
  LocalResource() = default;
  ~LocalResource() { for (auto handle : handles_) ::CloseHandle(handle); }
  LocalResource(const LocalResource&) = delete;
  LocalResource& operator=(const LocalResource&) = delete;
  bool Open(const std::filesystem::path& input) {
    if (!handles_.empty()) return false;
    auto value = input.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.size() < 4 || value.size() > 30000 || value[1] != L':' || value[2] != L'\\' ||
        !((value[0]>=L'A'&&value[0]<=L'Z')||(value[0]>=L'a'&&value[0]<=L'z'))) return false;
    std::vector<std::wstring> components;
    for(std::size_t begin_utf16=3;begin_utf16<value.size();) {
      auto end_utf16=value.find(L'\\',begin_utf16);
      auto part=value.substr(begin_utf16,end_utf16==value.npos?value.npos:end_utf16-begin_utf16);
      if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' '||
          part.find_first_of(L":<>\"|?*")!=part.npos) return false;
      components.push_back(part);
      if(end_utf16==value.npos)break;
      begin_utf16=end_utf16+1;
    }
    wchar_t device[1024]{};
    if(!::QueryDosDeviceW(value.substr(0,2).c_str(),device,1024))return false;
    const std::wstring name(device), prefix=L"\\Device\\HarddiskVolume";
    if(name.compare(0,prefix.size(),prefix)!=0 || name.size()==prefix.size() ||
      !std::all_of(name.begin()+static_cast<std::ptrdiff_t>(prefix.size()),name.end(),[](wchar_t unit){return unit>=L'0'&&unit<=L'9';}))return false;
    std::wstring current=L"\\\\?\\GLOBALROOT"+name+L"\\";
    if(!Pin(current,true))return false;
    for(std::size_t ordinal=0;ordinal<components.size();++ordinal){
      if(current.back()!=L'\\')current+=L'\\';
      current+=components[ordinal];
      if(!Pin(current,ordinal+1<components.size()))return false;
    }
    path_=current;return true;
  }
  const std::filesystem::path& path() const {return path_;}
 private:
  bool Pin(const std::wstring& path,bool directory){
    auto handle=::CreateFileW(path.c_str(),directory?FILE_READ_ATTRIBUTES:GENERIC_READ,
      FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL|
      (directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    try { handles_.push_back(handle); }
    catch (...) { ::CloseHandle(handle); throw; }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if(!::GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&info,sizeof(info)))return false;
    if(((info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory)return false;
    if(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){
      const bool cloud=(info.ReparseTag&~static_cast<DWORD>(IO_REPARSE_TAG_CLOUD_MASK))==IO_REPARSE_TAG_CLOUD;
      if(IsReparseTagNameSurrogate(info.ReparseTag)||!cloud)return false;
    }
    if(!directory&&(info.FileAttributes&(FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_RECALL_ON_OPEN|FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)))return false;
    return ::GetFileType(handle)==FILE_TYPE_DISK;
  }
  std::vector<HANDLE> handles_;
  std::filesystem::path path_;
};
}
#endif
