#include "PackageInstaller.hxx"
#include "../../common/PackageInventory.hxx"
#include <windows.h>
#include <vector>
#include <atomic>

namespace sf4e { namespace launcher {
namespace {
namespace fs = std::filesystem;
void CheckPath(const fs::path& root, const fs::path& relative) {
    auto path = root;
    for (const auto& part : relative) {
        path /= part;
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Reparse point in package destination");
    }
}
struct Change { fs::path relative; bool existed = false; };
}
bool InstallPackage(const fs::path& stagingInput, const fs::path& installInput, std::string& error) {
    std::vector<Change> changed;
    fs::path install, backup;
    try {
        install = fs::absolute(installInput).lexically_normal();
        const auto staging = fs::absolute(stagingInput).lexically_normal();
        if (!fs::is_directory(install) || !fs::is_directory(staging) || install == staging) throw std::runtime_error("Invalid install paths");
        CheckPath(install.root_path(),install.relative_path()); CheckPath(staging.root_path(),staging.relative_path());
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
            }
            changed.push_back({rel,existed});
        };
        for (const auto& rel : files) {
            preserve(rel); fs::create_directories((install/rel).parent_path());
            fs::copy_file(staging/rel,install/rel,fs::copy_options::overwrite_existing);
        }
        for (const auto* name : package::Obsolete) {
            const fs::path rel(name); CheckPath(install,rel);
            if (fs::exists(install/rel)) { preserve(rel); fs::remove(install/rel); }
        }
        error.clear(); return true;
    } catch (const std::exception& failure) {
        bool restored = true;
        for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
            std::error_code ec;
            if (it->existed) fs::copy_file(backup/it->relative, install/it->relative, fs::copy_options::overwrite_existing,ec);
            else fs::remove(install/it->relative,ec);
            if (ec) restored = false;
        }
        error = std::string(failure.what()) + (restored ? ". Previous files restored." : ". Automatic restore incomplete; preserved copies are in .ember-update-backups.");
        return false;
    }
}
} }
