#include "../common/GameCompatibility.hxx"
#include "test_support.hxx"
#include <string>

using namespace sf4e::compatibility;

int wmain(int argc, wchar_t** argv) {
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"s4c", 0, path));
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(file, "abc", 3, &written, NULL) && written == 3);
    char hash[65];
    CHECK(HashFile(file, hash) == 0);
    CHECK(std::string(hash) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CloseHandle(file);
    { GameFile game; CHECK(game.Open(path) == ERROR_REVISION_MISMATCH); }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size = {}; size.QuadPart = GameFileSize;
    CHECK(SetFilePointerEx(file, size, NULL, FILE_BEGIN)); CHECK(SetEndOfFile(file));
    CloseHandle(file);
    // A same-sized replacement must pass through hashing and still be refused.
    { GameFile game; CHECK(game.Open(path) == ERROR_REVISION_MISMATCH); }
    CHECK(DeleteFileW(path));
    { GameFile game; CHECK(game.Open(path) == ERROR_FILE_NOT_FOUND); }
    CHECK(HashFile(INVALID_HANDLE_VALUE, hash) == ERROR_INVALID_HANDLE);
    // Optional owned-game check, never bundled or required by CI. Only a
    // temporary copy is changed to verify rejection of a one-byte alteration.
    if (argc == 2) {
        {
            GameFile game;
            CHECK(game.Open(argv[1]) == ERROR_SUCCESS);
            HANDLE writer = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            CHECK(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
            CHECK(CopyFileW(argv[1], path, TRUE));
        }
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        CHECK(WriteFile(file, "X", 1, &written, NULL) && written == 1);
        CloseHandle(file);
        { GameFile game; CHECK(game.Open(path) == ERROR_REVISION_MISMATCH); }
        CHECK(DeleteFileW(path));
    }
    return 0;
}
