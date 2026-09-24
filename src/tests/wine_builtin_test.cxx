// The launcher's runtime check must recognise Wine's own msvcp140 and must
// not mistake a Microsoft-built DLL for it.
#include "../platform/WineBuiltin.hxx"
#include "temp_root.hxx"
#include "test_support.hxx"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static fs::path WriteStub(const fs::path& directory, const wchar_t* name, const char* stub) {
    std::string bytes(0x200, '\0');
    bytes[0] = 'M'; bytes[1] = 'Z';
    std::memcpy(&bytes[0x40], stub, std::strlen(stub));
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
}

int main() {
    using sf4e::platform::IsWineBuiltinDll;
    const fs::path root = MakeTempRoot(L"sf4e-wine-builtin-");
    fs::create_directories(root);
    CHECK(IsWineBuiltinDll(WriteStub(root, L"builtin.dll", "Wine builtin DLL").c_str()));
    CHECK(IsWineBuiltinDll(WriteStub(root, L"placeholder.dll", "Wine placeholder DLL").c_str()));
    CHECK(!IsWineBuiltinDll(WriteStub(root, L"microsoft.dll", "\x0e\x1f\xba\x0e\x00\xb4\x09\xcd!This program cannot be run in DOS mode.").c_str()));
    CHECK(!IsWineBuiltinDll((root / L"absent.dll").c_str()));
    // Too short to hold the stub.
    std::ofstream(root / L"short.dll", std::ios::binary) << "MZ";
    CHECK(!IsWineBuiltinDll((root / L"short.dll").c_str()));
    // This executable is not Wine's, whatever it runs on.
    wchar_t self[MAX_PATH] = {};
    CHECK(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0 && !IsWineBuiltinDll(self));
    RemoveTempRoot(root);
    std::puts("Wine builtin detection passed");
    return 0;
}
