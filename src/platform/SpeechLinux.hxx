#pragma once
#include <string>
#include <vector>

namespace sf4e { namespace speech {
// Under Proton (a Steam Deck) Wine has no SAPI voice, so speech-dispatcher,
// the Linux speech service, speaks. Wine has no Unix sockets and the game
// runs inside Steam's container (pressure-vessel), which shares neither
// speech-dispatcher's socket nor its programs. So Ember starts a small relay
// on the host once a session (RelayCommand) and writes SSIP, the service's
// text protocol, into a FIFO the relay reads. The relay holds one connection,
// so CANCEL SELF stops Ember's speech and never a screen reader's. It leaves
// when Ember closes the FIFO, so it ends with the game.

// The relay: connects to speech-dispatcher (starting it the way its own
// clients do when it is not running), makes the FIFO and, once it reads it,
// a ".ready" file beside it. Wine opens and writes a FIFO without a reader as
// if one were there, so only the ready file tells Ember the relay is there.
// It copies the FIFO into the connection until Ember closes it, by exiting or
// crashing, and leaves after 15 seconds when Ember never writes (Ember gives
// up after ten). A FIFO never written to does not read as closed, so it is
// opened without blocking and watched. On the way out, SIGTERM included, it
// removes both files. Replies are read and dropped.
constexpr const char* RelayScript = R"(import os, select, signal, socket, subprocess, sys, threading, time
fifo = sys.argv[1]
ready = fifo + ".ready"
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
path = os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()) + "/speech-dispatcher/speechd.sock"
def connect():
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    return s
try:
    s = connect()
except OSError:
    subprocess.run(["speech-dispatcher", "--spawn"])
    s = connect()
threading.Thread(target=lambda: [None for _ in iter(lambda: s.recv(4096), b"")], daemon=True).start()
try:
    os.mkfifo(fifo, 0o600)
    r = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    open(ready, "w").close()
    opened = False
    deadline = time.monotonic() + 15
    while opened or time.monotonic() < deadline:
        if not select.select([r], [], [], 1)[0]:
            continue
        data = os.read(r, 4096)
        if not data:
            break
        opened = True
        s.sendall(data)
finally:
    for name in (ready, fifo):
        if os.path.exists(name):
            os.unlink(name)
)";

// The SSIP for one request. Speech's own scales are mapped onto SSIP's -100
// to 100, the voice gets the language part of the interface's tag ("pt-BR"),
// and an empty text with interrupt only cancels. The text ends at a line
// holding only a dot, so a line of it that starts with one has it doubled.
inline std::string SsipMessage(const std::string& text, bool interrupt, int rate, int volume, const std::string& language) {
    std::string message = interrupt ? "CANCEL SELF\r\n" : "";
    if (text.empty()) return message;
    message += "SET SELF RATE " + std::to_string(rate * 10) + "\r\nSET SELF VOLUME " + std::to_string(volume * 2 - 100) +
        "\r\nSET SELF LANGUAGE " + language.substr(0, language.find('-')) + "\r\nSPEAK\r\n";
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        message += (!line.empty() && line[0] == '.' ? "." : "") + line + "\r\n";
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return message + ".\r\n";
}

// The Unix command line that starts the relay for `fifo`. Outside a container
// that is python3 itself. Inside one, the host's busctl, run by the host's
// loader from /run/host, asks the host's systemd over the shared session bus
// (bus is DBUS_SESSION_BUS_ADDRESS) to run it as the transient unit `unit`;
// KillMode=process leaves a speech-dispatcher the relay started running, and
// CollectMode drops the unit once it ends, failed or not.
inline std::vector<std::string> RelayCommand(bool container, const std::string& bus, const std::string& unit, const std::string& fifo) {
    if (!container) return {"/usr/bin/python3", "-c", RelayScript, fifo};
    return {"/run/host/usr/lib/ld-linux-x86-64.so.2", "--library-path", "/run/host/usr/lib:/run/host/usr/lib/systemd",
        "/run/host/usr/bin/busctl", "--address=" + bus, "--", "call", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "StartTransientUnit", "ssa(sv)a(sa(sv))", unit, "replace",
        "3", "KillMode", "s", "process", "CollectMode", "s", "inactive-or-failed", "ExecStart", "a(sasb)", "1", "/usr/bin/python3", "4", "python3", "-c", RelayScript, fifo, "false", "0"};
}
} }
