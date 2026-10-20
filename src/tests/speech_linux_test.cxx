// What Ember sends speech-dispatcher on a Steam Deck: an interruption cancels
// only Ember's own speech first, Stop only cancels, the scales map onto
// SSIP's, the voice gets the language part of the tag, a line of text that
// starts with a dot cannot end the message early, and the relay starts on the
// host through systemd from inside Steam's container.
#include "../platform/SpeechLinux.hxx"
#include "test_support.hxx"

using namespace sf4e::speech;
using Arguments = std::vector<std::string>;

int main() {
    CHECK(SsipMessage("Speech, On", true, -4, 60, "pt-BR") ==
        "CANCEL SELF\r\nSET SELF RATE -40\r\nSET SELF VOLUME 20\r\nSET SELF LANGUAGE pt\r\nSPEAK\r\nSpeech, On\r\n.\r\n");
    CHECK(SsipMessage("Settings", false, 10, 100, "en") == "SET SELF RATE 100\r\nSET SELF VOLUME 100\r\nSET SELF LANGUAGE en\r\nSPEAK\r\nSettings\r\n.\r\n");
    CHECK(SsipMessage("", true, 0, 0, "en") == "CANCEL SELF\r\n" && SsipMessage("", false, 0, 0, "en").empty());
    const auto dotted = SsipMessage("Up\n.\r\n..x", false, 0, 100, "en");
    CHECK(dotted.substr(dotted.find("SPEAK\r\n") + 7) == "Up\r\n..\r\n...x\r\n.\r\n");
    CHECK(RelayCommand(false, "", "", "/tmp/e.fifo") == Arguments({"/usr/bin/python3", "-c", RelayScript, "/tmp/e.fifo"}));
    const auto host = RelayCommand(true, "unix:path=/run/pressure-vessel/bus", "ember-speech-1.service", "/tmp/e.fifo");
    CHECK(host.size() == 32 && host[3] == "/run/host/usr/bin/busctl" && host[4] == "--address=unix:path=/run/pressure-vessel/bus" && host[5] == "--");
    CHECK(host[12] == "ember-speech-1.service" && host[14] == "3" && host[17] == "process" && host[20] == "inactive-or-failed");
    CHECK(Arguments(host.begin() + 22, host.end()) == Arguments({"a(sasb)", "1", "/usr/bin/python3", "4", "python3", "-c", RelayScript, "/tmp/e.fifo", "false", "0"}));
    return 0;
}
