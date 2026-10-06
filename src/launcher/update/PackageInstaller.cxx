#include "PackageInstaller.hxx"
#include "../../common/PackageInventory.hxx"
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <cwctype>
#include <cctype>
#include <memory>

namespace sf4e { namespace launcher {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr const wchar_t* TransactionName = UpdateTransactionName;
constexpr wchar_t LockName[] = L".ember-update.lock";
void CheckPath(const fs::path& root, const fs::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) throw std::runtime_error("Invalid update path");
    auto path = root;
    for (const auto& part : relative) {
        const auto name=part.wstring();
        if(name.empty() || name==L"." || name==L".." || name.back()==L'.' || name.back()==L' ' || name.find_first_of(L":*?\"<>|")!=std::wstring::npos)
            throw std::runtime_error("Invalid update path");
        path /= part;
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Reparse point in package destination");
    }
}
struct Change { fs::path relative; bool existed = false; };
bool SameHash(const std::string& left, const std::string& right) { return _stricmp(left.c_str(),right.c_str())==0; }
bool ValidHash(const std::string& value) {
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
}
std::wstring PathKey(const fs::path& path) {
    auto value=path.lexically_normal().wstring();
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return value;
}
// Every path a package of any version may contain: this build's files,
// optional ones, selection art, and the files earlier versions shipped.
bool IsProductPath(const fs::path& relative) {
    static const std::set<std::wstring> obsolete=[]{ std::set<std::wstring> keys; for(const auto* name:package::Obsolete) keys.insert(PathKey(name)); return keys; }();
    return package::IsAllowed(relative.c_str()) || obsolete.count(PathKey(relative))>0;
}
// The files this build's inventory names one by one, current or obsolete.
// Selection art is allowed by pattern instead, so a file there is Ember's only
// when a manifest names it; otherwise it is the player's.
bool InventoryNames(const fs::path& relative) {
    return IsProductPath(relative) && !selection::IsSelectionAssetPath(relative.wstring());
}
bool UpdaterState(const std::wstring& key) {
    return key==PathKey(TransactionName) || key==PathKey(std::wstring(TransactionName)+L".failed") || key==PathKey(LockName) || key.find(L".ember-update-backups\\")==0;
}
// Folders a removal left empty, up to but not including the install folder.
void RemoveEmptyParents(const fs::path& install, fs::path parent) {
    while(PathKey(parent).size()>PathKey(install).size() && RemoveDirectoryW(parent.c_str())) parent=parent.parent_path();
}
// Relative path and hash per MANIFEST.txt line ("<sha256>  <path>"): every
// file of its package but itself. Windows PowerShell starts it with a
// byte-order mark. Any other line means the file is not a package manifest.
std::map<std::wstring,std::pair<fs::path,std::string>> ReadManifest(const fs::path& manifest) {
    std::ifstream input(manifest,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot read the package manifest");
    std::map<std::wstring,std::pair<fs::path,std::string>> entries;
    bool first=true;
    for(std::string line; std::getline(input,line); first=false) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(first && line.rfind("\xEF\xBB\xBF",0)==0) line.erase(0,3);
        if(line.size()<67 || line.compare(64,2,"  ")!=0 || !ValidHash(line.substr(0,64))) throw std::runtime_error("Invalid package manifest");
        // PowerShell writes digests in uppercase; the journal and the final
        // check compare them as written, so they are kept lowercase here.
        std::string hash=line.substr(0,64);
        std::transform(hash.begin(),hash.end(),hash.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        const fs::path relative=fs::u8path(line.substr(66)).lexically_normal();
        if(relative.empty() || relative.is_absolute() || relative.has_root_name() || !entries.emplace(PathKey(relative),std::make_pair(relative,hash)).second)
            throw std::runtime_error("Invalid package manifest");
    }
    if(input.bad() || entries.empty()) throw std::runtime_error("Invalid package manifest");
    return entries;
}
fs::path TemporarySibling(const fs::path& path) {
    static std::atomic<unsigned> serial{0};
    return path.wstring()+L".ember-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++serial)+L".tmp";
}
void FlushFile(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush update file");
    const bool ok=FlushFileBuffers(file)!=FALSE; CloseHandle(file);
    if(!ok) throw std::runtime_error("Cannot flush update file");
}

std::string HashFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash update file");
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD objectBytes=0,digestBytes=0,used=0;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0 ||
        BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&used,0) < 0 ||
        BCryptGetProperty(algorithm,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&digestBytes),sizeof(digestBytes),&used,0) < 0)
        throw std::runtime_error("Cannot initialize update hash");
    std::vector<unsigned char> object(objectBytes), digest(digestBytes), buffer(64*1024);
    if (BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0) < 0) throw std::runtime_error("Cannot create update hash");
    while (input) { input.read(reinterpret_cast<char*>(buffer.data()),buffer.size()); const auto count=input.gcount();
        if(count>0 && BCryptHashData(hash,buffer.data(),static_cast<ULONG>(count),0)<0) throw std::runtime_error("Cannot hash update file"); }
    if (input.bad() || BCryptFinishHash(hash,digest.data(),digestBytes,0)<0) throw std::runtime_error("Cannot finish update hash");
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
    std::ostringstream out; out<<std::hex<<std::setfill('0'); for(auto byte:digest) out<<std::setw(2)<<static_cast<unsigned>(byte); return out.str();
}
void DurableJson(const fs::path& path, const json& value) {
    const auto temporary=TemporarySibling(path);
    const auto contents=value.dump(2);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write update transaction");
    DWORD written=0; const bool ok=WriteFile(file,contents.data(),static_cast<DWORD>(contents.size()),&written,nullptr)&&written==contents.size()&&FlushFileBuffers(file);
    CloseHandle(file); if(!ok || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit update transaction");
}
void ReplaceFileVerified(const fs::path& source,const fs::path& destination,const std::string& expected) {
    const auto temporary=TemporarySibling(destination);
    fs::create_directories(destination.parent_path());
    fs::copy_file(source,temporary); // Never overwrite an occupied temporary sibling.
    if(!SameHash(HashFile(temporary),expected)) throw std::runtime_error("Temporary update file failed verification");
    FlushFile(temporary);
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Atomic update replacement failed");
}
struct InstallLock {
    HANDLE handle=INVALID_HANDLE_VALUE; fs::path path;
    InstallLock()=default; InstallLock(const InstallLock&)=delete; InstallLock& operator=(const InstallLock&)=delete;
    InstallLock(InstallLock&& other) noexcept : handle(other.handle),path(std::move(other.path)){other.handle=INVALID_HANDLE_VALUE;}
    ~InstallLock(){if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);}
};
InstallLock Lock(const fs::path& install) {
    CheckPath(install,LockName);
    InstallLock lock; lock.path=install/LockName; lock.handle=CreateFileW(lock.path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock.handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Another Ember update or recovery is using this installation"); return lock;
}
// Once an install commits, older backup sets are no longer recovery evidence.
// Keep only the current set rather than one more per update (ledger H-013).
// Best effort: a cleanup failure must never turn a committed install into a
// reported rollback.
void PruneBackupSets(const fs::path& root, const fs::path& keep) noexcept {
    try {
        std::error_code error;
        for (fs::directory_iterator entry(root, error), end; !error && entry != end; entry.increment(error)) {
            std::error_code ignored;
            if (PathKey(entry->path()) != PathKey(keep)) fs::remove_all(entry->path(), ignored);
        }
    } catch (...) {}
}
// Moves a journal that can never be recovered out of the way, so the failure is
// reported once and later launches reach the game (v0.9.7 ignored the journal
// entirely). The file is kept beside the install as evidence when it can be
// renamed. False means the journal is still in place.
bool SetAsideJournal(const fs::path& transactionPath) noexcept {
    return MoveFileExW(transactionPath.c_str(),(transactionPath.wstring()+L".failed").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ||
        DeleteFileW(transactionPath.c_str());
}
// `ownRollback`: InstallPackage undoing its own failed install, so the folder
// is known to hold only its partial work and is always restored.
bool RecoverLocked(const fs::path& install, std::string& error, bool inspectOnly, bool ownRollback = false) {
    const auto transactionPath=install/TransactionName;
    CheckPath(install,TransactionName);
    if(!fs::exists(transactionPath)) return true;
    if(!fs::is_regular_file(transactionPath)) {error="Invalid update transaction path";return false;}
    if(inspectOnly){error="Pending update recovery is required";return false;}
    // Only a journal or backup that fails validation is set aside: nothing has
    // changed yet and no retry can succeed. A read or restore failure keeps the
    // journal, since a later attempt can still finish the restore.
    bool validating=true;
    try {
        std::ifstream stream(transactionPath); json transaction; stream>>transaction;
        if(transaction.value("schema",0)!=1 || PathKey(fs::u8path(transaction.value("installation",std::string())))!=PathKey(install) ||
            (transaction.value("state",std::string())!="prepared" && transaction.value("state",std::string())!="committed") ||
            !transaction.at("operations").is_array() || transaction.at("operations").empty() || !transaction.at("target").is_object())
            throw std::runtime_error("Invalid pending update transaction");
        const fs::path backup=fs::u8path(transaction.at("backup").get<std::string>());
        if(!backup.is_absolute() || (PathKey(backup.parent_path())!=PathKey(install/L".ember-update-backups") &&
            !(PathKey(backup.parent_path())==PathKey(install.parent_path()) && backup.filename().wstring().find(L"Ember-backup-")==0)))
            throw std::runtime_error("Invalid update backup location");
        CheckPath(backup.root_path(),backup.relative_path());
        // "committed" is written only after every installed file matched the
        // target, so only its removal failed. Files that differ now were
        // changed afterwards, by the player; they are not ours to undo.
        const bool committed=transaction.at("state")=="committed";
        std::set<std::wstring> seen;
        std::map<std::wstring,std::string> targets;
        for(const auto& item:transaction.at("target").items()) {
            const auto relative=fs::u8path(item.key()); CheckPath(install,relative);
            if(!ValidHash(item.value().get<std::string>()) || !targets.emplace(PathKey(relative),item.value().get<std::string>()).second)
                throw std::runtime_error("Invalid target inventory");
        }
        // Validate the entire journal before changing any destination. Damaged
        // evidence must never produce a partial restore.
        for(const auto& operation:transaction.at("operations")) {
            const auto relative=fs::u8path(operation.at("path").get<std::string>()); CheckPath(install,relative); CheckPath(backup,relative);
            const auto key=PathKey(relative);
            if(!seen.insert(key).second || key==PathKey(TransactionName) || key==PathKey(LockName) || key.find(L".ember-update-backups") == 0 ||
                fs::is_directory(install/relative)) throw std::runtime_error("Invalid recovery operation");
            const bool existed=operation.at("existed").get<bool>();
            const auto prior=operation.at("priorSha256").get<std::string>();
            if((existed && !ValidHash(prior)) || (!existed && !prior.empty())) throw std::runtime_error("Invalid prior hash");
            // A backup that disagrees with its journal entry means damaged
            // evidence, not a folder the player replaced.
            if(!committed && existed && fs::is_regular_file(backup/relative) && !SameHash(HashFile(backup/relative),prior))
                throw std::runtime_error("Update backup is missing or damaged");
        }
        // A prepared journal restores only a half-applied update. If every file
        // already matches the target, the update finished. If any file matches
        // neither the prior nor the target bytes, the folder was replaced since,
        // such as by extracting a package over it, and restoring the old backup
        // would silently downgrade it. A missing file (antivirus, say) is not
        // such evidence and is restored.
        validating=false;
        bool finished=!ownRollback, replaced=false;
        if(!committed && !ownRollback) for(const auto& operation:transaction.at("operations")) {
            const fs::path relative=fs::u8path(operation.at("path").get<std::string>());
            const bool present=fs::is_regular_file(install/relative);
            const std::string current=present?HashFile(install/relative):std::string();
            const bool existed=operation.at("existed").get<bool>();
            const bool atPrior=existed?present&&SameHash(current,operation.at("priorSha256").get<std::string>()):!present;
            const auto target=targets.find(PathKey(relative));
            const bool atTarget=target!=targets.end()?present&&SameHash(current,target->second):!present;
            finished=finished&&atTarget;
            replaced=replaced||(present&&!atPrior&&!atTarget);
        }
        if(!committed && !finished) {
            if(replaced) {
                stream.close();
                // Reporting success with the journal still in place would make
                // the Launcher and Updater restart each other forever.
                if(!SetAsideJournal(transactionPath)) throw std::runtime_error("Cannot clear the pending update transaction");
                error.clear(); return true;
            }
            // A half-applied update needs every backup; one that is missing
            // can never be restored.
            for(const auto& operation:transaction.at("operations"))
                if(operation.at("existed").get<bool>() && !fs::is_regular_file(backup/fs::u8path(operation.at("path").get<std::string>()))) {
                    validating=true; throw std::runtime_error("Update backup is missing or damaged");
                }
            for(const auto& operation:transaction.at("operations")) {
                const fs::path relative=fs::u8path(operation.at("path").get<std::string>());
                const auto prior=operation.at("priorSha256").get<std::string>();
                if(operation.at("existed").get<bool>()) {
                    // Untouched so far (a removal that failed, say) needs no restore,
                    // and may still be held open.
                    if(fs::is_regular_file(install/relative) && SameHash(HashFile(install/relative),prior)) continue;
                    ReplaceFileVerified(backup/relative,install/relative,prior);
                } else { fs::remove(install/relative); }
            }
            for(const auto& operation:transaction.at("operations")) {
                const fs::path relative=fs::u8path(operation.at("path").get<std::string>());
                if(operation.at("existed").get<bool>()) {
                    if(!fs::is_regular_file(install/relative) || !SameHash(HashFile(install/relative),operation.at("priorSha256").get<std::string>()))
                        throw std::runtime_error("Restored update file failed verification");
                } else if(fs::exists(install/relative)) throw std::runtime_error("New update file could not be removed during recovery");
            }
        }
        stream.close();
        fs::remove(transactionPath); error.clear(); return true;
    } catch(const std::exception& failure){
        error=failure.what();
        // An own rollback keeps its journal, so the next launch retries it.
        if(validating && !ownRollback) SetAsideJournal(transactionPath);
        return false;
    }
}
}
bool RecoverPackage(const fs::path& installInput, std::string& error, bool inspectOnly) {
    try { const auto install=fs::absolute(installInput).lexically_normal(); CheckPath(install.root_path(),install.relative_path());
        if(inspectOnly) return RecoverLocked(install,error,true);
        auto lock=Lock(install); return RecoverLocked(install,error,false); }
    catch(const std::exception& failure){error=failure.what();return false;}
}
bool UninstallPackage(const fs::path& installInput, std::string& error) {
    try {
        const auto install=fs::absolute(installInput).lexically_normal();
        CheckPath(install.root_path(),install.relative_path());
        if(!fs::is_directory(install)) throw std::runtime_error("Invalid install paths");
        std::vector<fs::path> parents;
        std::string failed; // Keep going past a file in use; report it at the end.
        {
            auto lock=Lock(install);
            // The installed version's own manifest as well as this build's
            // inventory: setup runs the helper it shipped, which may be older
            // than the version updates have installed since. Read under the
            // lock, and removed last, only once everything else is gone, so a
            // retry after a failure still knows what is ours.
            // A manifest that is there but cannot be trusted or read stops the
            // uninstall before anything goes: it may be all that names a file.
            std::set<std::wstring> named;
            CheckPath(install,L"MANIFEST.txt");
            if(fs::exists(fs::symlink_status(install/L"MANIFEST.txt"))) for(const auto& [key,entry]:ReadManifest(install/L"MANIFEST.txt")) named.insert(key);
            const auto ours=[&](const fs::path& rel){ const auto key=PathKey(rel); return named.count(key) || InventoryNames(rel) || UpdaterState(key); };
            std::vector<fs::path> owned;
            for(fs::recursive_directory_iterator entry(install), end; entry!=end; ++entry) {
                const auto rel=entry->path().lexically_relative(install);
                // A junction or symlink is the player's, whatever it points at.
                if(GetFileAttributesW(entry->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) { entry.disable_recursion_pending(); continue; }
                if(entry->is_directory() || !ours(rel) || PathKey(rel)==PathKey(LockName) || PathKey(rel)==L"manifest.txt") continue;
                CheckPath(install,rel);
                owned.push_back(rel);
            }
            const auto removeOwned=[&](const fs::path& rel){
                std::error_code code; fs::remove(install/rel,code);
                if(code && failed.empty()) failed="Cannot remove "+rel.u8string()+": "+code.message();
                parents.push_back((install/rel).parent_path());
            };
            for(const auto& rel:owned) removeOwned(rel);
            if(failed.empty() && fs::exists(install/L"MANIFEST.txt")) { CheckPath(install,L"MANIFEST.txt"); removeOwned(L"MANIFEST.txt"); }
        }
        CheckPath(install,LockName); fs::remove(install/LockName);
        for(const auto& parent:parents) RemoveEmptyParents(install,parent);
        RemoveDirectoryW(install.c_str()); // Only when nothing of the player's is left.
        if(!failed.empty()) throw std::runtime_error(failed);
    } catch(const std::exception& failure){error=failure.what();return false;}
    error.clear(); return true;
}
// The required and obsolete paths a package's own PackageInventory.inc
// declares, as preflight reads them, so a package of any version is held to
// its own list rather than this build's.
struct Inventory { std::map<std::wstring,fs::path> required; std::set<std::wstring> obsolete; };
// Every line is blank, a // comment, or exactly one declaration such as
// SF4E_PACKAGE_REQUIRED("notices\\Discord-SDK.txt"), spaces allowed around it.
// Anything else is refused rather than skipped, so a line this reader cannot
// follow never quietly drops a requirement the compiled inventory keeps.
Inventory ReadInventory(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot read the package inventory");
    Inventory inventory;
    bool first=true;
    for(std::string line; std::getline(input,line); first=false) {
        if(first && line.rfind("\xEF\xBB\xBF",0)==0) line.erase(0,3);
        // Lines end in LF or CRLF; any other CR could start a line another
        // reader sees and this one would not, even inside a comment.
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.find('\r')!=std::string::npos) throw std::runtime_error("Invalid package inventory");
        const auto begin=line.find_first_not_of(" \t"), end=line.find_last_not_of(" \t");
        if(begin==std::string::npos || line.compare(begin,2,"//")==0) continue;
        line=line.substr(begin,end-begin+1);
        const auto open=line.find("(\"");
        if(line.rfind("SF4E_PACKAGE_",0)!=0 || open==std::string::npos || line.size()<open+4 || line.compare(line.size()-2,2,"\")")!=0)
            throw std::runtime_error("Invalid package inventory");
        const std::string kind=line.substr(13,open-13);
        // The path literal holds plain characters and doubled backslashes
        // only, so it means here exactly what it means to the compiler: no
        // other escape, no control character, no "." or ".." part.
        const std::string literal=line.substr(open+2,line.size()-open-4);
        std::string text;
        for(size_t at=0; at<literal.size(); ++at) {
            const unsigned char c=static_cast<unsigned char>(literal[at]);
            if(c<0x20 || c==0x7f || c=='"' || c=='/') throw std::runtime_error("Invalid package inventory");
            if(c=='\\' && (++at>=literal.size() || literal[at]!='\\')) throw std::runtime_error("Invalid package inventory");
            text+=static_cast<char>(c);
        }
        const fs::path relative=fs::u8path(text);
        if(text.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) throw std::runtime_error("Invalid package inventory");
        for(const auto& part:relative) if(part.empty() || part==L"." || part==L"..") throw std::runtime_error("Invalid package inventory");
        if(kind=="REQUIRED") inventory.required.emplace(PathKey(relative),relative);
        else if(kind=="OBSOLETE") inventory.obsolete.insert(PathKey(relative));
        else if(kind!="OPTIONAL") throw std::runtime_error("Invalid package inventory");
    }
    if(input.bad() || inventory.required.empty()) throw std::runtime_error("Invalid package inventory");
    return inventory;
}
// The package's files by relative path, each with its manifest hash, once
// every file matched the manifest and the manifest named nothing missing.
static std::map<std::wstring,std::pair<fs::path,std::string>> VerifiedPackage(const fs::path& package) {
    CheckPath(package.root_path(),package.relative_path());
    if(!fs::is_directory(package)) throw std::runtime_error("Invalid package folder");
    auto manifest=ReadManifest(package/L"MANIFEST.txt");
    std::set<std::wstring> seen;
    for(fs::recursive_directory_iterator entry(package), end; entry!=end; ++entry) {
        const auto relative=entry->path().lexically_relative(package);
        CheckPath(package,relative);
        if(entry->is_directory()) continue;
        if(!entry->is_regular_file() || !IsProductPath(relative)) throw std::runtime_error("Unexpected package file");
        const auto key=PathKey(relative);
        if(key==PathKey(L"MANIFEST.txt")) continue;
        const auto named=manifest.find(key);
        if(named==manifest.end()) throw std::runtime_error("Package file not in its manifest");
        if(!SameHash(HashFile(entry->path()),named->second.second)) throw std::runtime_error("Package file failed verification");
        seen.insert(key);
    }
    for(const auto& [key,named]:manifest) if(!seen.count(key)) throw std::runtime_error("Incomplete package");
    // Matching its manifest is not having what it needs: a package also meets
    // its own inventory's required list and carries none of its obsolete files.
    const auto inventory=ReadInventory(package/L"PackageInventory.inc");
    seen.insert(PathKey(L"MANIFEST.txt"));
    for(const auto& [key,relative]:inventory.required) if(!seen.count(key)) throw std::runtime_error("Incomplete package: "+relative.u8string());
    for(const auto& key:seen) if(inventory.obsolete.count(key)) throw std::runtime_error("Package carries an obsolete file");
    if(!seen.count(PathKey(L"Launcher.exe")) || !seen.count(PathKey(L"Updater.exe"))) throw std::runtime_error("Incomplete package");
    manifest[PathKey(L"MANIFEST.txt")]={fs::path(L"MANIFEST.txt"),HashFile(package/L"MANIFEST.txt")};
    return manifest;
}
std::string Sha256Hex(const fs::path& file) { return HashFile(file); }
bool ValidatePackageFolder(const fs::path& packageInput, std::string& error) {
    try { VerifiedPackage(fs::absolute(packageInput).lexically_normal()); error.clear(); return true; }
    catch(const std::exception& failure){error=failure.what();return false;}
}
bool InstallPackage(const fs::path& stagingInput, const fs::path& installInput, std::string& error) {
    std::vector<Change> changed;
    fs::path install, backup;
    std::unique_ptr<InstallLock> installLock;
    bool prepared=false;
    try {
        install = fs::absolute(installInput).lexically_normal();
        const auto staging = fs::absolute(stagingInput).lexically_normal();
        if (!fs::is_directory(install) || !fs::is_directory(staging) || install == staging) throw std::runtime_error("Invalid install paths");
        CheckPath(install.root_path(),install.relative_path());
        installLock=std::make_unique<InstallLock>(Lock(install));
        if(!RecoverLocked(install,error,false)) throw std::runtime_error(error);
        const auto package=VerifiedPackage(staging);
        std::vector<fs::path> files, removals;
        for(const auto& [key,named]:package) { CheckPath(install,named.first); files.push_back(named.first); }
        // Removals are what this folder owns and the package lacks: the files
        // the installed MANIFEST.txt names, plus the obsolete list for folders
        // from before manifests. Never merely a permitted path, so art or a
        // doc the player keeps at an accepted name is not an update's to take.
        // A folder from before manifests has only the obsolete list. A manifest
        // that is there but cannot be read stops the update, like uninstall:
        // replacing it would lose for good which files this folder owns.
        std::map<std::wstring,fs::path> owned;
        for(const auto* name:package::Obsolete) owned.emplace(PathKey(name),fs::path(name));
        CheckPath(install,L"MANIFEST.txt");
        if(fs::exists(fs::symlink_status(install/L"MANIFEST.txt")))
            for(const auto& [key,named]:ReadManifest(install/L"MANIFEST.txt")) owned.emplace(key,named.first);
        for(const auto& [key,relative]:owned) {
            if(package.count(key) || !IsProductPath(relative) || !fs::exists(fs::symlink_status(install/relative))) continue;
            CheckPath(install,relative);
            if(!fs::is_regular_file(install/relative)) throw std::runtime_error("Destination is not a file");
            removals.push_back(relative);
        }
        std::stable_sort(files.begin(),files.end(),[](const fs::path& left,const fs::path& right){
            return _wcsicmp(left.c_str(),L"MANIFEST.txt")==0 ? false : _wcsicmp(right.c_str(),L"MANIFEST.txt")==0;
        });
        static std::atomic<unsigned> serial{0};
        backup = install / L".ember-update-backups" / (std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial));
        CheckPath(install,backup.lexically_relative(install));
        fs::create_directories(backup);
        const auto preserve = [&](const fs::path& rel) {
            CheckPath(install,rel);
            const bool existed = fs::exists(install/rel);
            if (existed) {
                if (!fs::is_regular_file(install/rel)) throw std::runtime_error("Destination is not a file");
                fs::create_directories((backup/rel).parent_path());
                fs::copy_file(install/rel,backup/rel);
                if(!SameHash(HashFile(install/rel),HashFile(backup/rel))) throw std::runtime_error("Backup verification failed");
                FlushFile(backup/rel);
            }
            changed.push_back({rel,existed});
        };
        for (const auto& rel : removals) preserve(rel);
        for (const auto& rel : files) preserve(rel);
        json operations=json::array(), target=json::object();
        for(const auto& change:changed) operations.push_back({{"path",change.relative.generic_u8string()},{"existed",change.existed},
            {"priorSha256",change.existed?HashFile(backup/change.relative):std::string()}});
        for(const auto& rel:files) target[rel.generic_u8string()]=package.at(PathKey(rel)).second;
        json transaction={{"schema",1},{"state","prepared"},{"installation",install.u8string()},{"backup",backup.u8string()},
            {"operations",operations},{"target",target}};
        DurableJson(install/TransactionName,transaction);
        prepared=true;
        int completed=0; const char* terminateAfter=std::getenv("SF4E_UPDATE_TEST_TERMINATE_AFTER");
        for (const auto& rel : removals) fs::remove(install/rel);
        for (const auto& rel : removals) RemoveEmptyParents(install,(install/rel).parent_path());
        for (const auto& rel : files) {
            ReplaceFileVerified(staging/rel,install/rel,target.at(rel.generic_u8string()).get<std::string>());
            if(terminateAfter && ++completed==std::atoi(terminateAfter)) TerminateProcess(GetCurrentProcess(),86);
        }
        for(const auto& item:target.items()) if(HashFile(install/fs::u8path(item.key()))!=item.value().get<std::string>()) throw std::runtime_error("Installed update verification failed");
        transaction["state"]="committed"; DurableJson(install/TransactionName,transaction);
        if(terminateAfter && strcmp(terminateAfter,"commit")==0) TerminateProcess(GetCurrentProcess(),86);
        fs::remove(install/TransactionName);
    } catch (const std::exception& failure) {
        bool restored = true;
        std::string recoveryError;
        if(prepared && installLock) restored=RecoverLocked(install,recoveryError,false,true);
        error = std::string(failure.what()) + (!prepared ? ". No new update was applied; preserve any pending recovery evidence." : restored ? ". Previous files restored." : ". Automatic restore incomplete; preserve the transaction and backup. " + recoveryError);
        return false;
    }
    PruneBackupSets(install/L".ember-update-backups", backup);
    error.clear(); return true;
}
} }
