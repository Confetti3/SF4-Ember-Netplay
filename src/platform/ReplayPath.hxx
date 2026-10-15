#pragma once
#include "../common/ReplayLink.hxx"
#include "Utf8.hxx"
#include <climits>
#include <cstring>
#include <utility>

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
// on it names it (without "\\?\"): "" when path or what it leads to is not on
// a local drive, cannot be opened, or is not a folder (folder) or a regular
// file (!folder). held keeps the handle open until the caller closes it. It
// reads the folder's listing or the file's data and shares no deletion, so
// while it is open the folder or file cannot be moved, deleted or replaced.
inline std::wstring ResolveLocalPath(const std::wstring& path, bool folder, HANDLE* held = nullptr) noexcept {
 if(held)*held=INVALID_HANDLE_VALUE;
 if(!OnLocalDrive(path))return {}; // reject mapped network drives before opening
 const bool holding=held!=nullptr;
 const DWORD access=FILE_READ_ATTRIBUTES|(holding?FILE_READ_DATA:0);
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

// Whether inner looks to be outer or under it, both as ResolveLocalPath gives
// them, compared a whole name at a time without regard to case. Only a guide
// to where to look: names alike in all but case can be two folders (a
// case-sensitive folder), so identity decides (ArchiveFolderOf).
inline bool WithinFolder(const std::wstring& inner, std::wstring outer) noexcept {
 while(!outer.empty()&&outer.back()==L'\\')outer.pop_back();
 if(outer.empty()||inner.size()<outer.size()||outer.size()>INT_MAX)return false;
 const int length=static_cast<int>(outer.size());
 return CompareStringOrdinal(inner.c_str(),length,outer.c_str(),length,TRUE)==CSTR_EQUAL&&
  (inner.size()==outer.size()||inner[outer.size()]==L'\\');
}

// Whether two handles are the same file or folder: the same volume and the
// same file identity on it, whatever names reach them.
inline bool SameFile(HANDLE a, HANDLE b) noexcept {
 FILE_ID_INFO first={}, second={};
 if(GetFileInformationByHandleEx(a,FileIdInfo,&first,sizeof first)&&GetFileInformationByHandleEx(b,FileIdInfo,&second,sizeof second))
  return first.VolumeSerialNumber==second.VolumeSerialNumber&&std::memcmp(&first.FileId,&second.FileId,sizeof first.FileId)==0;
 BY_HANDLE_FILE_INFORMATION one={}, other={};
 return GetFileInformationByHandle(a,&one)&&GetFileInformationByHandle(b,&other)&&one.dwVolumeSerialNumber==other.dwVolumeSerialNumber&&
  one.nFileIndexHigh==other.nFileIndexHigh&&one.nFileIndexLow==other.nFileIndexLow;
}

// A replay's folder in the replay archive, checked and held for the shell to
// open (ArchiveFolderOf). While it is held, the replay cannot be deleted or
// moved, so the folder can be neither emptied nor turned into a junction in
// place, and the folder itself cannot be moved, deleted or replaced. Empty
// when the replay is not in a folder of the archive. Move-only; the handles
// close with it.
class ArchiveFolder {
public:
 ArchiveFolder() = default;
 ArchiveFolder(ArchiveFolder&& other) noexcept { *this=std::move(other); }
 ArchiveFolder& operator=(ArchiveFolder&& other) noexcept {
  if(this!=&other){Close();path_=std::move(other.path_);folder_=other.folder_;replay_=other.replay_;other.folder_=other.replay_=INVALID_HANDLE_VALUE;other.path_.clear();}
  return *this;
 }
 ArchiveFolder(const ArchiveFolder&) = delete;
 ArchiveFolder& operator=(const ArchiveFolder&) = delete;
 ~ArchiveFolder() { Close(); }
 const std::wstring& Path() const { return path_; }
 bool Empty() const { return path_.empty(); }
private:
 void Close() noexcept {
  for(HANDLE* handle:{&folder_,&replay_})if(*handle!=INVALID_HANDLE_VALUE){CloseHandle(*handle);*handle=INVALID_HANDLE_VALUE;}
  path_.clear();
 }
 std::wstring path_;
 HANDLE folder_=INVALID_HANDLE_VALUE, replay_=INVALID_HANDLE_VALUE;
 friend ArchiveFolder ArchiveFolderOf(const std::wstring& file, const std::wstring& archive) noexcept;
};

// The folder that holds file, a replay in the replay archive at archive, as
// handles reach it, held (ArchiveFolder): empty when there is no archive, or
// file is not a replay on a local drive, or, once junctions and links are
// followed, it is not in a folder of the archive. Checked when asked, not when
// the archive was listed. The archive is found among the folder's ancestors
// by name only to know which ancestor to look at; that ancestor must be the
// archive itself, the same folder by identity, so a sibling whose name
// differs only in case (a case-sensitive folder) is not taken for it.
inline ArchiveFolder ArchiveFolderOf(const std::wstring& file, const std::wstring& archive) noexcept {
 ArchiveFolder found;
 HANDLE root=INVALID_HANDLE_VALUE, ancestor=INVALID_HANDLE_VALUE;
 try {
  if(!archive.empty()&&replay_link::IsReplayPath(WideToUtf8(file))) {
   const std::wstring rootPath=ResolveLocalPath(archive,true,&root);
   const std::wstring replay=ResolveLocalPath(file,false,&found.replay_);
   const auto slash=replay.find_last_of(L'\\');
   if(!rootPath.empty()&&replay_link::IsReplayPath(WideToUtf8(replay))&&slash!=std::wstring::npos) {
    const std::wstring folder=ResolveLocalPath(replay.substr(0,slash==2?3:slash),true,&found.folder_);
    std::wstring outer=rootPath;while(outer.size()>3&&outer.back()==L'\\')outer.pop_back();
    if(!folder.empty()&&WithinFolder(folder,outer)) {
     ResolveLocalPath(folder.substr(0,outer.size()),true,&ancestor);
     if(ancestor!=INVALID_HANDLE_VALUE&&SameFile(ancestor,root))found.path_=folder;
    }
   }
  }
 } catch(...) {}
 for(HANDLE handle:{root,ancestor})if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
 if(found.path_.empty())found.Close();
 return found;
}
} }
