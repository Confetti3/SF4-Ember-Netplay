#include "../platform/CustomContentFiles.hxx"
#include "../common/StageCatalog.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

namespace fs = std::filesystem;
namespace selection = sf4e::selection;
using sf4e::platform::custom_content::Scan;

struct Fixture {
    fs::path root;
    Fixture() {
        wchar_t temp[MAX_PATH], unique[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, temp));
        CHECK(GetTempFileNameW(temp, L"s4c", 0, unique));
        CHECK(DeleteFileW(unique));
        root = unique;
        CHECK(fs::create_directory(root));
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void File(const fs::path& name, const std::string& bytes = "asset") {
        const fs::path path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        CHECK(out.write(bytes.data(), bytes.size()));
        out.close(); CHECK(out);
    }
    void Colors(const std::string& costume) {
        for (int color = 1; color <= 10; ++color) {
            const auto name = "chara/RYU/" + costume + "_" + (color < 10 ? "0" : "") + std::to_string(color);
            File(name + ".col.emb"); File(name + ".obj.emm");
        }
    }
};

int main() {
    Fixture f;
    CHECK(Scan(L"")->stages.empty());
    CHECK(Scan((f.root / L"missing").wstring())->Costumes(0).empty());
    CHECK(Scan(f.root.wstring())->Costumes(0).empty());

    f.File("chara/RYU/RYU_08.obj.emo");
    f.Colors("RYU_08");
    fs::remove(f.root / "chara/RYU/RYU_08_10.obj.emm");
    fs::create_directory(f.root / "chara/RYU/RYU_08_10.obj.emm");
    CHECK(!Scan(f.root.wstring())->HasCostume(0, 7));
    fs::remove(f.root / "chara/RYU/RYU_08_10.obj.emm");
    f.File("chara/RYU/RYU_08_10.obj.emm");
    f.File("chara/RYU/RYU_08.txt", std::string("\xEF\xBB\xBF") + "Custom\t outfit  \r\nsecond line");

    f.File("chara/RYU/RYU_09.obj.emo"); f.Colors("RYU_09");
    f.File("chara/RYU/RYU_09.txt", std::string(38, 'a') + "\xF0\x9F\x98\x80" + std::string(10000, 'b'));
    f.File("chara/RYU/RYU_10.obj.emo"); // incomplete costume
    f.File("chara/RYU/RYU_100.obj.emo"); // malformed slot
    fs::create_directory(f.root / "chara/RYU/RYU_11.obj.emo"); f.Colors("RYU_11");

    f.File("chara/RYU/RYU_01_30.col.emb"); f.File("chara/RYU/RYU_01_30.obj.emm");
    f.File("chara/RYU/RYU_01_30.txt", std::string(1, '\xFF')); // invalid UTF-8 has no label
    f.File("chara/RYU/RYU_01_99.col.emb"); f.File("chara/RYU/RYU_01_99.obj.emm");
    f.File("chara/RYU/RYU_01_99.txt", std::string(10000, 'x'));
    f.File("chara/RYU/RYU_01_29.col.emb"); f.File("chara/RYU/RYU_01_29.obj.emm");
    f.File("chara/RYU/RYU_01_31.col.emb");
    f.File("chara/RYU/RYU_01_32.col.emb"); fs::create_directory(f.root / "chara/RYU/RYU_01_32.obj.emm");
    f.File("chara/RYU/RYU_08_30.col.emb"); f.File("chara/RYU/RYU_08_30.obj.emm");
    f.File("chara/RYU/RYU_10_30.col.emb"); f.File("chara/RYU/RYU_10_30.obj.emm");

    f.File("stage/STG_C12.emz"); f.File("stage/STG_C12.tex.emz");
    f.File("stage/STG_C12.txt", "A complete stage\nignored");
    f.File("stage/STG_C11.emz");
    f.File("stage/STG_JUR.emz"); f.File("stage/STG_JUR.tex.emz"); // stock code
    fs::create_directory(f.root / "stage/STG_D12.emz"); f.File("stage/STG_D12.tex.emz");
    f.File("stage/STG_E12.emz"); fs::create_directory(f.root / "stage/STG_E12.tex.emz");
    f.File("stage/STG_1234.emz"); f.File("stage/STG_1234.tex.emz");

    const auto installed = Scan(f.root.wstring());
    CHECK((installed->Costumes(0) == std::vector<int>{7, 8}));
    CHECK(installed->Name(0, 7) == "Custom outfit");
    CHECK(installed->Name(0, 8) == std::string(38, 'a'));
    CHECK((installed->Colors(0, 0) == std::vector<int>{29, 98}));
    CHECK(installed->Name(0, 0, 29).empty());
    CHECK(installed->Name(0, 0, 98) == std::string(40, 'x'));
    CHECK(installed->HasColor(0, 7, 29));
    CHECK(!installed->HasColor(0, 9, 29));
    const int stage = selection::CustomStageId("C12");
    CHECK((installed->stages == std::vector<int>{stage}));
    CHECK(installed->StageName(stage) == "A complete stage");
    // A catalog already published to readers never changes after files change.
    fs::remove(f.root / "stage/STG_C12.tex.emz");
    fs::remove(f.root / "chara/RYU/RYU_08_10.obj.emm");
    const auto removed = Scan(f.root.wstring());
    CHECK(!removed->HasStage(stage) && !removed->HasCostume(0, 7));
    CHECK(installed->HasStage(stage) && installed->HasCostume(0, 7));
    return 0;
}
