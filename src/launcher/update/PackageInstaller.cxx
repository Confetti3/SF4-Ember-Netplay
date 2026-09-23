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
#include <algorithm>
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
bool RecoverLocked(const fs::path& install, std::string& error, bool inspectOnly) {
    const auto transactionPath=install/TransactionName;
    CheckPath(install,TransactionName);
    if(!fs::exists(transactionPath)) return true;
    if(!fs::is_regular_file(transactionPath)) {error="Invalid update transaction path";return false;}
    if(inspectOnly){error="Pending update recovery is required";return false;}
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
        std::set<std::wstring> seen, targets;
        for(const auto& item:transaction.at("target").items()) {
            const auto relative=fs::u8path(item.key()); CheckPath(install,relative);
            if(!targets.insert(PathKey(relative)).second || !ValidHash(item.value().get<std::string>())) throw std::runtime_error("Invalid target inventory");
        }
        // Validate the entire journal and all required backups before changing
        // any destination. Damaged evidence must never produce a partial restore.
        for(const auto& operation:transaction.at("operations")) {
            const auto relative=fs::u8path(operation.at("path").get<std::string>()); CheckPath(install,relative); CheckPath(backup,relative);
            const auto key=PathKey(relative);
            if(!seen.insert(key).second || key==PathKey(TransactionName) || key==PathKey(LockName) || key.find(L".ember-update-backups") == 0 ||
                fs::is_directory(install/relative)) throw std::runtime_error("Invalid recovery operation");
            const bool existed=operation.at("existed").get<bool>();
            const auto prior=operation.at("priorSha256").get<std::string>();
            if((existed && !ValidHash(prior)) || (!existed && !prior.empty())) throw std::runtime_error("Invalid prior hash");
            if(transaction.at("state")=="prepared" && existed &&
                (!fs::is_regular_file(backup/relative) || !SameHash(HashFile(backup/relative),prior))) throw std::runtime_error("Update backup is missing or damaged");
        }
        if(transaction.value("state",std::string())=="committed") {
            for(const auto& item:transaction.at("target").items()) if(!fs::is_regular_file(install/fs::u8path(item.key())) || !SameHash(HashFile(install/fs::u8path(item.key())),item.value().get<std::string>()))
                throw std::runtime_error("Committed update does not match its target");
            for(const auto& operation:transaction.at("operations")) {
                const auto relative=fs::u8path(operation.at("path").get<std::string>());
                if(!targets.count(PathKey(relative)) && fs::exists(install/relative)) throw std::runtime_error("Obsolete file remains in committed update");
            }
        } else {
            for(const auto& operation:transaction.at("operations")) {
                const fs::path relative=fs::u8path(operation.at("path").get<std::string>());
                if(operation.at("existed").get<bool>()) {
                    ReplaceFileVerified(backup/relative,install/relative,operation.at("priorSha256").get<std::string>());
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
    } catch(const std::exception& failure){error=failure.what();return false;}
}
}
bool RecoverPackage(const fs::path& installInput, std::string& error, bool inspectOnly) {
    try { const auto install=fs::absolute(installInput).lexically_normal(); CheckPath(install.root_path(),install.relative_path());
        if(inspectOnly) return RecoverLocked(install,error,true);
        auto lock=Lock(install); return RecoverLocked(install,error,false); }
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
        CheckPath(install.root_path(),install.relative_path()); CheckPath(staging.root_path(),staging.relative_path());
        installLock=std::make_unique<InstallLock>(Lock(install));
        if(!RecoverLocked(install,error,false)) throw std::runtime_error(error);
        // No transaction is pending, so earlier backup sets are no longer
        // recovery evidence. Keep only the one this install creates, instead
        // of one more set per update forever (ledger H-013).
        { std::error_code ignored; fs::remove_all(install/L".ember-update-backups",ignored); }
        for (const auto* required : package::Required) if (!fs::is_regular_file(staging/required)) throw std::runtime_error("Incomplete package");
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(staging)) {
            const auto rel = entry.path().lexically_relative(staging);
            CheckPath(staging,rel); CheckPath(install,rel);
            if (!entry.is_directory()) {
                if (!entry.is_regular_file() || !package::IsAllowed(rel.c_str())) throw std::runtime_error("Unexpected package file");
                files.push_back(rel);
            }
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
        for (const auto& rel : files) preserve(rel);
        for (const auto* name : package::Obsolete) { const fs::path rel(name); CheckPath(install,rel); if(fs::exists(install/rel)) preserve(rel); }
        json operations=json::array(), target=json::object();
        for(const auto& change:changed) operations.push_back({{"path",change.relative.generic_u8string()},{"existed",change.existed},
            {"priorSha256",change.existed?HashFile(backup/change.relative):std::string()}});
        for(const auto& rel:files) target[rel.generic_u8string()]=HashFile(staging/rel);
        json transaction={{"schema",1},{"state","prepared"},{"installation",install.u8string()},{"backup",backup.u8string()},
            {"operations",operations},{"target",target}};
        DurableJson(install/TransactionName,transaction);
        prepared=true;
        int completed=0; const char* terminateAfter=std::getenv("SF4E_UPDATE_TEST_TERMINATE_AFTER");
        for (const auto* name : package::Obsolete) {
            const fs::path rel(name); CheckPath(install,rel);
            if (fs::exists(install/rel)) fs::remove(install/rel);
        }
        for (const auto& rel : files) {
            ReplaceFileVerified(staging/rel,install/rel,target.at(rel.generic_u8string()).get<std::string>());
            if(terminateAfter && ++completed==std::atoi(terminateAfter)) TerminateProcess(GetCurrentProcess(),86);
        }
        for(const auto& item:target.items()) if(HashFile(install/fs::u8path(item.key()))!=item.value().get<std::string>()) throw std::runtime_error("Installed update verification failed");
        transaction["state"]="committed"; DurableJson(install/TransactionName,transaction); fs::remove(install/TransactionName);
        error.clear(); return true;
    } catch (const std::exception& failure) {
        bool restored = true;
        std::string recoveryError;
        if(prepared && installLock) restored=RecoverLocked(install,recoveryError,false);
        error = std::string(failure.what()) + (!prepared ? ". No new update was applied; preserve any pending recovery evidence." : restored ? ". Previous files restored." : ". Automatic restore incomplete; preserve the transaction and backup. " + recoveryError);
        return false;
    }
}
} }
