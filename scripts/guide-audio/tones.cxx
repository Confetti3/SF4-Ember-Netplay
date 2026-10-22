// Writes the notice chime and the description tick, from the repository's own definitions
// (src/common/AwayAlert.hxx), as chime.wav and tick.wav. Built by scripts/guide-audio/make-guide-audio.sh.
#include "common/AwayAlert.hxx"
#include <cstdio>
static void Save(const char* name, const std::vector<std::uint8_t>& wave) {
    if (FILE* file = std::fopen(name, "wb")) { std::fwrite(wave.data(), 1, wave.size(), file); std::fclose(file); }
}
int main() {
    Save("chime.wav", sf4e::away_alert::NoticeChime(60));
    Save("tick.wav", sf4e::away_alert::DescriptionTick());
}
