#pragma once

#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay { namespace json_file {

constexpr std::size_t MaximumBytes = 256 * 1024;

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool ReadBytes(const std::filesystem::path& path, std::string& bytes,
    bool& missing, std::string& error);
bool Parse(const std::string& bytes, nlohmann::json& value, std::string& error);
bool WriteNew(const std::filesystem::path& path, const std::string& bytes,
    std::string& error);
bool PreserveBackup(const std::filesystem::path& path, const std::string& original,
    std::string& error);
bool Publish(const std::filesystem::path& directory, const std::wstring& filename,
    const nlohmann::json& document, std::string& error);

} } } // namespace sf4e::netplay::json_file
