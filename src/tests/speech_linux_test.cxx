// The lines Ember writes the speech relay on a Steam Deck (SAY, RENDER), each
// part after its clip key; the key, which names a clip by language, rate and
// text; the relay's status; and the command that starts the relay on the
// host through systemd from inside Steam's container.
#include "../platform/SpeechLinux.hxx"
#include "test_support.hxx"

using namespace sf4e::speech;
using Arguments = std::vector<std::string>;

int main() {
    const auto on = ClipKey("en", 0, "On");
    CHECK(on.size() == 16 && on == ClipKey("en", 0, "On"));
    CHECK(on != ClipKey("en", 1, "On") && on != ClipKey("de", 0, "On") && on != ClipKey("en", 0, "Off"));
    // The fields are kept apart, so "e" + "n0" is not "en" + "0".
    CHECK(ClipKey("e", 10, "x") != ClipKey("e1", 0, "x"));
    CHECK(SayLine({"Lower input lag", "On"}, true, -4, 60, "pt-BR") == "SAY\t1\t-4\t60\tpt-BR\t" + ClipKey("pt-BR", -4, "Lower input lag") +
        "\tLower input lag\t" + ClipKey("pt-BR", -4, "On") + "\tOn\n");
    // A sound after the parts goes last, under the key "wav".
    CHECK(SayLine({"On"}, false, 0, 100, "en", "/tmp/tick.wav") == "SAY\t0\t0\t100\ten\t" + ClipKey("en", 0, "On") + "\tOn\twav\t/tmp/tick.wav\n");
    CHECK(RenderLine({"Speech"}, 0, "en") == "RENDER\t0\ten\t" + ClipKey("en", 0, "Speech") + "\tSpeech\n");
    // A part cannot break the line or its fields; its key is of the text as written.
    CHECK(SayLine({"a\tb\nc"}, false, 0, 100, "en") == "SAY\t0\t0\t100\ten\t" + ClipKey("en", 0, "a\tb\nc") + "\ta b c\n");

    CHECK(ParseRelayStatus("natural").voice == Voice::Natural && ParseRelayStatus("failed").voice == Voice::Failed);
    const auto downloading = ParseRelayStatus("downloading 42");
    CHECK(downloading.voice == Voice::Downloading && downloading.percent == 42);
    CHECK(ParseRelayStatus("").voice == Voice::System && ParseRelayStatus("system").voice == Voice::System && ParseRelayStatus("downloading 101").voice == Voice::System);

    CHECK(RelayCommand(false, "", "", "/e/speech_relay.py", "/tmp/e.fifo") == Arguments({"/usr/bin/python3", "/e/speech_relay.py", "/tmp/e.fifo"}));
    const auto host = RelayCommand(true, "unix:path=/run/pressure-vessel/bus", "ember-speech-1.service", "/e/speech_relay.py", "/tmp/e.fifo");
    CHECK(host.size() == 31 && host[3] == "/run/host/usr/bin/busctl" && host[4] == "--address=unix:path=/run/pressure-vessel/bus" && host[5] == "--");
    CHECK(host[12] == "ember-speech-1.service" && host[14] == "3" && host[17] == "process" && host[20] == "inactive-or-failed");
    CHECK(Arguments(host.begin() + 22, host.end()) == Arguments({"a(sasb)", "1", "/usr/bin/python3", "3", "python3", "/e/speech_relay.py", "/tmp/e.fifo", "false", "0"}));
    return 0;
}
