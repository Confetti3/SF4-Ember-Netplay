#pragma once
#include "../common/ReplayLink.hxx"
#include "Utf8.hxx"

namespace sf4e { namespace platform {
inline std::string ResolveReplayFile(const std::string& requested) noexcept {
 return replay_link::ResolveReplayPath(requested, [](const std::string& file) {
  const auto local = [](const std::wstring& path) {
   if(path.size()<3||path[1]!=L':')return false;
   const wchar_t root[]={path[0],L':',L'\\',0};
   const auto type=GetDriveTypeW(root);
   return type==DRIVE_FIXED||type==DRIVE_REMOVABLE||type==DRIVE_RAMDISK||type==DRIVE_CDROM;
  };
  const auto wide=Utf8ToWide(file.c_str());
  if(!local(wide))return replay_link::ReplayTarget{}; // reject mapped network drives before opening
  HANDLE handle=CreateFileW(wide.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(handle==INVALID_HANDLE_VALUE)return replay_link::ReplayTarget{};
  BY_HANDLE_FILE_INFORMATION info={};
  wchar_t final[2048]={};
  const bool regular=GetFileType(handle)==FILE_TYPE_DISK&&GetFileInformationByHandle(handle,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);
  const DWORD length=regular?GetFinalPathNameByHandleW(handle,final,2048,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
  CloseHandle(handle);
  if(!length||length>=2048)return replay_link::ReplayTarget{};
  std::wstring resolved(final,length);
  if(resolved.compare(0,4,L"\\\\?\\")==0)resolved.erase(0,4);
  return replay_link::ReplayTarget(WideToUtf8(resolved),local(resolved),regular);
 });
}
} }
