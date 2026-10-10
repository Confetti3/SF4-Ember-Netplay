#pragma once
#include "../common/ReplayLink.hxx"
#include "Utf8.hxx"
#include <climits>

namespace sf4e { namespace platform {
// A drive of this PC: fixed, removable, RAM or optical. Mapped network
// drives and UNC paths are not.
inline bool OnLocalDrive(const std::wstring& path) noexcept {
 if(path.size()<3||path[1]!=L':')return false;
 const wchar_t root[]={path[0],L':',L'\\',0};
 const auto type=GetDriveTypeW(root);
 return type==DRIVE_FIXED||type==DRIVE_REMOVABLE||type==DRIVE_RAMDISK||type==DRIVE_CDROM;
}

// Where path leads once junctions and links are followed, as a handle opened
// on it names it (without "\\?\"):"" when path or what it leads to is not on
// a local drive, cannot be opened, or is not a folder (folder) or a regular
// file (!folder). held, for a folder, keeps the handle open until the caller
// closes it. It reads the folder's listing and shares no deletion, so while
// it is open the folder cannot be moved, deleted or replaced.
inline std::wstring ResolveLocalPath(const std::wstring& path, bool folder, HANDLE* held = nullptr) noexcept {
 if(held)*held=INVALID_HANDLE_VALUE;
 if(!OnLocalDrive(path))return {}; // reject mapped network drives before opening
 const bool holding=held&&folder;
 const DWORD access=FILE_READ_ATTRIBUTES|(holding?FILE_LIST_DIRECTORY:0);
 const DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|(holding?0:FILE_SHARE_DELETE);
 HANDLE handle=CreateFileW(path.c_str(),access,share,nullptr,OPEN_EXISTING,folder?FILE_FLAG_BACKUP_SEMANTICS:FILE_ATTRIBUTE_NORMAL,nullptr);
 if(handle==INVALID_HANDLE_VALUE)return {};
 BY_HANDLE_FILE_INFORMATION info={};
 bool kind=GetFileType(handle)==FILE_TYPE_DISK&&GetFileInformationByHandle(handle,&info);
 if(kind) {
  const bool directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  kind=folder?directory:!directory;
 }
 wchar_t final[2048]={};
 const DWORD length=kind?GetFinalPathNameByHandleW(handle,final,2048,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
 std::wstring resolved;
 if(length&&length<2048) {
  resolved.assign(final,length);
  if(resolved.compare(0,4,L"\\\\?\\")==0)resolved.erase(0,4);
  if(!OnLocalDrive(resolved))resolved.clear();
 }
 if(holding&&!resolved.empty())*held=handle;
 else CloseHandle(handle);
 return resolved;
}

inline std::string ResolveReplayFile(const std::string& requested) noexcept {
 return replay_link::ResolveReplayPath(requested, [](const std::string& file) {
  const auto resolved=ResolveLocalPath(Utf8ToWide(file.c_str()),false);
  return resolved.empty()?replay_link::ReplayTarget{}:replay_link::ReplayTarget(WideToUtf8(resolved),true,true);
 });
}

// Whether inner is outer or lies under it, both as ResolveLocalPath gives
// them, compared a whole name at a time and, as Windows names are, without
// regard to case.
inline bool WithinFolder(const std::wstring& inner, std::wstring outer) noexcept {
 while(!outer.empty()&&outer.back()==L'\\')outer.pop_back();
 if(outer.empty()||inner.size()<outer.size()||outer.size()>INT_MAX)return false;
 const int length=static_cast<int>(outer.size());
 return CompareStringOrdinal(inner.c_str(),length,outer.c_str(),length,TRUE)==CSTR_EQUAL&&
  (inner.size()==outer.size()||inner[outer.size()]==L'\\');
}

// The folder that holds file, a replay in the replay archive at archive, as
// a handle reaches it: "" when there is no archive, or file is not a replay
// on a local drive, or once junctions and links are followed it is not in a
// folder of the archive. Checked when asked, not when the archive was listed.
// held keeps that folder open, so it cannot be moved or replaced while it is
// opened, until the caller closes it; INVALID_HANDLE_VALUE with "".
inline std::wstring ArchiveFolderOf(const std::wstring& file, const std::wstring& archive, HANDLE& held) noexcept {
 held=INVALID_HANDLE_VALUE;
 try {
  if(archive.empty())return {};
  const std::wstring root=ResolveLocalPath(archive,true);
  const std::wstring resolved=Utf8ToWide(ResolveReplayFile(WideToUtf8(file)).c_str());
  const auto slash=resolved.find_last_of(L'\\');
  if(root.empty()||slash==std::wstring::npos)return {};
  const std::wstring folder=ResolveLocalPath(resolved.substr(0,slash==2?3:slash),true,&held);
  if(!folder.empty()&&WithinFolder(folder,root))return folder;
 } catch(...) {}
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
 held=INVALID_HANDLE_VALUE;
 return {};
}
} }
