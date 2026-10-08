#include "../training/ClgFile.hxx"
#include "game_file_test_support.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace sf4e::clg;

static bool Refused(const Bytes& bytes) {
    File model;
    std::string error;
    return !ReadExact(bytes, bytes.size(), model, error) && !error.empty() && model.levels.empty();
}

static File Sample() {
    File file;
    for (std::size_t i = 0; i < GameLevels; ++i) {
        Level level;
        level.name = "Lv_" + std::to_string(i + 1);
        level.ultra = i % 2;
        level.dummy = static_cast<std::int32_t>(i % 5);
        for (std::size_t j = 0; j <= i % GameSteps; ++j) {
            Step step;
            step.screen = {{"ID_CMD_CMN_0004", "ID_CMD_CMN_0024", "ID_CMD_CMN_0006", ""}};
            step.help = {{"ID_CMD_CMN_0004", "ID_CMD_CMN_0024", "ID_CMD_CMN_0042", ""}};
            for (std::size_t k = 0; k <= j; ++k) step.criteria.push_back(static_cast<std::int32_t>(26 + i + k));
            level.steps.push_back(step);
        }
        file.levels.push_back(level);
    }
    Level& odd = file.levels[3];
    odd.name = "new_lv4";
    odd.menuA = 88;
    odd.menuB = 304;
    odd.field40 = 5;
    odd.field4C = 3;
    odd.setup = {{1, 1, -1, -1, 0, 1}};
    odd.flags = 0;
    odd.steps[0].type = Script;
    odd.steps[0].screen = {{"ID_CMD_CMN_0022", "", "ID_CMD_CMN_0001", std::string(MaxText, 'x')}};
    odd.steps[0].criteria = {325, -1};
    return file;
}

int main() {
    const File sample = Sample();
    Bytes bytes, again;
    File back;
    std::string error;

    // A written file reads back as the same model and writes the same bytes.
    CHECK(Write(sample, bytes, error));
    std::size_t steps = 0, criteria = 0;
    for (const Level& level : sample.levels) {
        steps += level.steps.size();
        for (const Step& step : level.steps) criteria += step.criteria.size();
    }
    CHECK(bytes.size() == 0x40 + GameLevels * 0xB0 + steps * 0x108 + criteria * 4);
    CHECK(ReadExact(bytes, bytes.size(), back, error) && back == sample);
    CHECK(Write(back, again, error) && again == bytes);

    // The blocks sit where the game looks for them.
    CHECK(std::memcmp(bytes.data(), "#CLG\xFE\xFF\x40\x00\x01\x00", 10) == 0);
    CHECK(bytes[0x30] == GameLevels && bytes[0x34] == 0x40 && bytes[0x38] == 0 && bytes[0x3C] == 0);
    CHECK(bytes[0x40 + 0x5C] == 0x80 && bytes[0x40 + 0x5D] == 0x0A);    // level 1 to setup 1 at 0xAC0
    CHECK(bytes[0x40 + 0x6C] == 0x00 && bytes[0x40 + 0x6D] == 0x0F);    // level 1 to task list 1 at 0xF40
    CHECK(bytes[0xF40 + 0x0C] == 0x80 && bytes[0xF40 + 0x0D] == 0x01);  // task list 1 to step 1 at 0x10C0
    CHECK(std::memcmp(&bytes[0x10C0], "ID_CMD_CMN_0004", 16) == 0 && bytes[0x10C0 + 0x100] == 1 && bytes[0x10C0 + 0x102] == 1);

    // Extra records follow the criteria, with their offset counted from 0x38.
    File extra = sample;
    extra.extras.resize(2);
    extra.extras[0].a = 3; extra.extras[0].b = 1; extra.extras[0].c = 2; extra.extras[0].d = -1;
    extra.extras[1].a = 10; extra.extras[1].b = 5; extra.extras[1].c = 6; extra.extras[1].d = -1;
    CHECK(Write(extra, again, error) && again.size() == bytes.size() + 32);
    CHECK(again[0x38] == 2 && 0x38 + (again[0x3C] | again[0x3D] << 8) == static_cast<int>(bytes.size()));
    CHECK(ReadExact(again, again.size(), back, error) && back == extra);
    for (std::size_t size = 0; size < again.size(); ++size) CHECK(!ReadExact(again, size, back, error));

    // An empty file is a header alone.
    CHECK(Write(File(), again, error) && again.size() == 0x40);
    CHECK(ReadExact(again, again.size(), back, error) && back == File());

    // Every truncation is refused, and a refused read leaves the model alone.
    back = sample;
    for (std::size_t size = 0; size < bytes.size(); ++size) CHECK(!ReadExact(bytes, size, back, error) && !error.empty());
    CHECK(back == sample);
    CHECK(!Read(nullptr, 0, back, error));

    // Offsets and counts that leave the file are refused.
    const std::size_t level = 0x40, task = 0xF40, step = 0x10C0;
    const std::uint32_t wild[] = {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, static_cast<std::uint32_t>(bytes.size()), 0xFFFFFF00u};
    for (std::size_t field : {std::size_t(0x30), std::size_t(0x34), level + 0x5C, level + 0x6C, task + 0x08, task + 0x0C, step + 0x104}) {
        for (std::uint32_t value : wild) {
            Bytes bad = bytes;
            Set32(bad, field, value);
            CHECK(Refused(bad));
        }
    }
    {
        Bytes bad = bytes;
        bad[step + 0x102] = bad[step + 0x103] = 0xFF;
        CHECK(Refused(bad));
        bad = bytes;
        Set32(bad, 0x38, 1);
        Set32(bad, 0x3C, static_cast<std::uint32_t>(bytes.size() - 0x38));
        CHECK(Refused(bad));
        bad = bytes;
        Set32(bad, 0x3C, 4);
        CHECK(Refused(bad));
        bad = bytes;
        std::memset(&bad[step], 'A', 32);       // a text id without its NUL
        CHECK(Refused(bad));
        bad = bytes;
        bad[step + 20] = 'A';                   // bytes after the NUL
        CHECK(Refused(bad));
        bad = bytes;
        bad[level] = 0x80;
        CHECK(Refused(bad));
        bad = bytes;
        bad[0] = 'X';
        CHECK(Refused(bad));
        bad = bytes;
        bad.resize(MaxBytes + 1);
        CHECK(Refused(bad));
    }

    // Every level pointed at the largest step list would grow the model far
    // past the file, so shared steps are refused.
    {
        Bytes bad = bytes;
        for (std::size_t i = 0; i < GameLevels; ++i) {
            Set32(bad, task + i * 0x10 + 0x08, static_cast<std::uint32_t>((bytes.size() - step) / 0x108));
            Set32(bad, task + i * 0x10 + 0x0C, static_cast<std::uint32_t>(step - (task + i * 0x10)));
        }
        CHECK(Refused(bad));
    }

    // Any single damaged byte either reads or is refused, never out of range.
    // The step and criteria blocks are mostly text, so one value does there.
    const unsigned char damage[] = {0xFF, 0x00, 0x01, 0x7F, 0x80};
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        for (std::size_t pick = 0; pick < (at < step ? sizeof damage : 1); ++pick) {
            const unsigned char value = damage[pick];
            Bytes bad = bytes;
            bad[at] = value;
            File model;
            if (ReadExact(bad, bad.size(), model, error)) CHECK(model.levels.size() <= bad.size() / 0x70);
        }
    }

    // A model the format cannot hold is refused and leaves the bytes alone.
    again = bytes;
    File bad = sample;
    bad.levels[0].steps[0].help[2] = std::string(MaxText + 1, 'x');
    CHECK(!Write(bad, again, error) && !error.empty() && again == bytes);
    bad = sample;
    bad.levels[0].steps[0].screen[0] = "caf\xC3\xA9";
    CHECK(!Write(bad, again, error));
    bad = sample;
    bad.levels[0].name = std::string(MaxName + 1, 'x');
    CHECK(!Write(bad, again, error));
    bad.levels[0].name = std::string(MaxName, 'x');
    CHECK(Write(bad, again, error) && ReadExact(again, again.size(), back, error) && back == bad);
    bad = sample;
    bad.levels[0].steps[0].criteria.assign(MaxCriteria + 1, 1);
    CHECK(!Write(bad, again, error));
    bad.levels[0].steps[0].criteria.assign(MaxCriteria, 1);
    CHECK(Write(bad, again, error) && ReadExact(again, again.size(), back, error) && back == bad);
    bad = sample;
    bad.levels.resize(MaxBytes / 0x70);
    CHECK(!Write(bad, again, error));

    // With the game installed, every stock file is rebuilt byte for byte.
    if (const char* game = std::getenv("SF4E_GAME_DIR")) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::size_t files = 0;
        for (fs::recursive_directory_iterator it(game, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".clg") continue;
            std::ifstream stream(it->path(), std::ios::binary);
            const Bytes stock((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            File model;
            if (!ReadExact(stock, stock.size(), model, error)) {
                std::fprintf(stderr, "%s: %s\n", it->path().string().c_str(), error.c_str());
                CHECK(false);
            }
            CHECK(Write(model, again, error) && again == stock);
            CHECK(model.levels.size() == GameLevels);
            for (const Level& stockLevel : model.levels) CHECK(!stockLevel.steps.empty() && stockLevel.steps.size() <= GameSteps);
            ++files;
        }
        CHECK(files > 0);
        std::printf("%zu stock trial files rebuilt byte for byte\n", files);
    }
    return 0;
}
