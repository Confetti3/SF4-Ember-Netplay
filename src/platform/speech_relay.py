#!/usr/bin/env python3
# Ember's voice on Linux and the Steam Deck (src/platform/SpeechLinux.hxx).
# Ember starts it on the host once a session with the FIFO to read, and talks
# to it in lines of tab-separated fields:
#   SAY <interrupt 0|1> <rate -10..10> <volume 0..100> <language tag> (<key> <text>)...
#   RENDER <rate> <language tag> (<key> <text>)...
#   STOP
# It speaks with a Piper voice for the language, downloaded once into
# ~/.cache/ember-speech and checked against its pinned SHA-256, and until then
# (or for a language without a voice) through speech-dispatcher. Every clip
# Piper makes is kept under Ember's key for it, so a part said once is ready at
# once; RENDER makes clips ahead at low priority. Stopping only ever stops
# Ember's own speech. It tells Ember how it speaks in <fifo>.status: system,
# downloading <percent>, natural or failed; and that it reads the FIFO by
# making <fifo>.ready. It leaves when Ember closes the FIFO, and after 15
# seconds when Ember never writes.
import hashlib, json, os, queue, select, signal, socket, subprocess, sys, tarfile, threading, time, urllib.request

FIFO = sys.argv[1]
READY, STATUS = FIFO + ".ready", FIFO + ".status"
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "ember-speech")
PIPER_DIR = os.path.join(CACHE, "piper")
PIPER = ("https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz", 26460462,
         "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992")
VOICE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/c10ece1aade47bb51c153c893d14e5bf8e5b7117/"
# Ember's language tag to a Piper voice: its path, model size, model and config SHA-256.
# Japanese and Korean have none this Piper can speak, and stay on speech-dispatcher.
VOICES = {
    "en": ("en/en_US/lessac/medium/en_US-lessac-medium", 63201294, "5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f", "efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0"),
    "de": ("de/de_DE/thorsten/medium/de_DE-thorsten-medium", 63201294, "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819", "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85"),
    "fr": ("fr/fr_FR/siwis/medium/fr_FR-siwis-medium", 63201294, "641d1ab097da2b81128c076810edb052b385decc8be3381814802a64a73baf99", "39479916c2db192b5ac9764daddd0c744d83e023ad890c6976c0633ae4df8959"),
    "es-ES": ("es/es_ES/davefx/medium/es_ES-davefx-medium", 63201294, "6658b03b1a6c316ee4c265a9896abc1393353c2d9e1bca7d66c2c442e222a917", "0e0dda87c732f6f38771ff274a6380d9252f327dca77aa2963d5fbdf9ec54842"),
    "es-419": ("es/es_MX/ald/medium/es_MX-ald-medium", 63201294, "019b3803293c93e34a206dd2e53a3889209a514e786fd7144f7b70196c579b63", "5a71498158e04afc8099bfd019c7e87c68eb9d042505a2b1a87e5c1ac2b1a61d"),
    "it": ("it/it_IT/paola/medium/it_IT-paola-medium", 63511038, "6fc918b5a0ea6137382833dddfa567bffbe6a5060c02043c87192ee59c04210c", "aea19c0a7fce29fbc359b93f10e7902854401e4c95ae2ea328ae516b15d296cf"),
    "nl": ("nl/nl_BE/nathalie/medium/nl_BE-nathalie-medium", 63201294, "49cf48023861f9fd42e13a8632f068fee67d1ce244a6ee38f29595afbf0a6be4", "4704af2736022e910a3f32672480d5530dd39da5c2bcc079f315f604166ff0de"),
    "pl": ("pl/pl_PL/gosia/medium/pl_PL-gosia-medium", 63201294, "38f66464240ed74f186e6b7dc13c6e3b22e023426299f25c2b3cc9dfa9373fbc", "1aefb31a9d53ffe44a8163ff73ec833acb7a6253848f6bb0403d8a66f9c7510d"),
    "pt-BR": ("pt/pt_BR/faber/medium/pt_BR-faber-medium", 63201294, "858555e3a064209c57088fe6bd70c4c3dc54d03eaa00c45d5ecaf43a33f95aa7", "7e694de195ae3fc36dd732c445eb04fb49b649854893cb5506b978f0d50a1d6f"),
    "ru": ("ru/ru_RU/irina/medium/ru_RU-irina-medium", 63201294, "8ff38212d23da300bbe3705c645e6e5b9475f0bfde01558eb17813e22acaaaaa", "c2ec28bb38e2b59e93b959b3e40348c1afebbd272f30fed5d41205d08e98a9d7"),
    "cs": ("cs/cs_CZ/jirka/medium/cs_CZ-jirka-medium", 63201294, "cbd5c900acacc8e8cbecd64347abb8de39c00a9d3104bed06fee92e4f319efc8", "fb38b1799b7354808227c065efa97b1ffa2b0cde59505babb56a36d35af9c637"),
    "zh-Hans": ("zh/zh_CN/huayan/medium/zh_CN-huayan-medium", 63201294, "9929917bf8cabb26fd528ea44d3a6699c11e87317a14765312420be230be0f3d", "d521dc45504a8ccc99e325822b35946dd701840bfb07e3dbb31a40929ed6a82b"),
}


def status(text):
    with open(STATUS + ".tmp", "w") as f:
        f.write(text)
    os.replace(STATUS + ".tmp", STATUS)


def model(language):
    """The voice's model file when it is downloaded, else None."""
    voice = VOICES.get(language)
    path = voice and os.path.join(CACHE, "voices", os.path.basename(voice[0]) + ".onnx")
    ok = path and os.path.exists(path) and os.path.exists(path + ".json") and os.path.exists(os.path.join(PIPER_DIR, "piper"))
    return path if ok else None


def fetch(url, path, size, sha256, progress):
    """Downloads url to path through a .part file, checking its SHA-256."""
    digest = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=30) as response, open(path + ".part", "wb") as out:
        for chunk in iter(lambda: response.read(1 << 16), b""):
            digest.update(chunk)
            out.write(chunk)
            progress(len(chunk))
    if digest.hexdigest() != sha256:
        os.unlink(path + ".part")
        raise ValueError("SHA-256 mismatch for " + url)
    os.replace(path + ".part", path)


def download(language):
    """Piper and the language's voice, once; the status says how far it is."""
    name, size, model_sha, config_sha = VOICES[language]
    voices = os.path.join(CACHE, "voices")
    os.makedirs(voices, exist_ok=True)
    total, done = size + (0 if os.path.exists(os.path.join(PIPER_DIR, "piper")) else PIPER[1]), [0]
    def progress(count):
        done[0] += count
        status("downloading %d" % min(99, done[0] * 100 // total))
    status("downloading 0")
    try:
        if not os.path.exists(os.path.join(PIPER_DIR, "piper")):
            archive = os.path.join(CACHE, "piper.tar.gz")
            fetch(PIPER[0], archive, PIPER[1], PIPER[2], progress)
            with tarfile.open(archive) as tar:
                tar.extractall(CACHE, filter="data")
            os.unlink(archive)
        target = os.path.join(voices, os.path.basename(name) + ".onnx")
        fetch(VOICE_URL + name + ".onnx.json", target + ".json", 0, config_sha, lambda count: None)
        fetch(VOICE_URL + name + ".onnx", target, size, model_sha, progress)
        status("natural")
    except Exception as error:
        print("Ember speech: no voice download:", error, file=sys.stderr)
        status("failed")


class Piper:
    """One Piper process for a voice at a rate, making clips one at a time."""
    def __init__(self, path, rate, low):
        self.key = (path, rate)
        self.process = subprocess.Popen(
            [os.path.join(PIPER_DIR, "piper"), "-q", "--model", path, "--json-input", "--sentence_silence", "0",
             "--length_scale", "%.3f" % (1 / (1 + rate * 0.05))],
            cwd=PIPER_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            preexec_fn=(lambda: os.nice(10)) if low else None)
        children.append(self.process)

    def make(self, text, clip):
        """Writes the clip; False when Piper has died."""
        part = "%s.%d.part" % (clip, id(self))
        try:
            self.process.stdin.write((json.dumps({"text": text, "output_file": part}) + "\n").encode())
            self.process.stdin.flush()
            if not self.process.stdout.readline():
                return False
            os.replace(part, clip)
            return True
        except OSError:
            return False


def clip_path(path, key):
    folder = os.path.join(CACHE, "clips", os.path.basename(path)[:-5])
    os.makedirs(folder, exist_ok=True)
    return os.path.join(folder, key + ".wav")


def connect_dispatcher():
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
    s.sendall(b"SET SELF CLIENT_NAME user:ember:menus\r\n")
    return s


signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
dispatcher = connect_dispatcher()
said, renders = queue.Queue(), queue.Queue()
lock = threading.Lock()
generation, player, broken, downloading = 0, None, set(), set()
# Piper and paplay end with the relay; a speech-dispatcher it started does not.
children = []


def stop():
    """Drops what Ember asked to say and silences only Ember's own voice."""
    global generation
    with lock:
        generation += 1
        if player:
            player.kill()
    for waiting in (said, renders):
        while not waiting.empty():
            waiting.get_nowait()
    dispatcher.sendall(b"CANCEL SELF\r\n")


def voice(language):
    """The model to speak the language with now, starting its download the first time."""
    path = None if language in broken else model(language)
    if path:
        status("natural")
    elif language in VOICES and language not in broken and language not in downloading:
        downloading.add(language)
        threading.Thread(target=download, args=(language,), daemon=True).start()
    elif language not in VOICES:
        status("system")
    return path


def speak():
    global player
    piper = None
    while True:
        heard, rate, volume, language, parts = said.get()
        path = voice(language)
        if not path:
            text = ", ".join(text for _, text in parts)
            dispatcher.sendall(("SET SELF RATE %d\r\nSET SELF VOLUME %d\r\nSET SELF LANGUAGE %s\r\nSPEAK\r\n%s%s\r\n.\r\n" % (
                rate * 10, volume * 2 - 100, language.split("-")[0], "." if text.startswith(".") else "", text)).encode())
            continue
        if not piper or piper.key != (path, rate):
            piper = Piper(path, rate, False)
        for key, text in parts:
            clip = clip_path(path, key)
            if heard != generation:
                break
            if not os.path.exists(clip) and not piper.make(text, clip):
                broken.add(language)
                piper = None
                said.put((heard, rate, volume, language, parts))
                break
            with lock:
                if heard != generation:
                    break
                player = subprocess.Popen(["paplay", "--volume=%d" % (volume * 65536 // 100), clip])
                children[:] = [child for child in children if child.poll() is None] + [player]
            player.wait()


def render():
    piper = None
    while True:
        rate, language, parts = renders.get()
        path = None if language in broken else model(language)
        if not path:
            continue
        if not piper or piper.key != (path, rate):
            piper = Piper(path, rate, True)
        for key, text in parts:
            clip = clip_path(path, key)
            if not os.path.exists(clip) and not piper.make(text, clip):
                piper = None
                break


def handle(line):
    fields = line.split("\t")
    pairs = lambda start: list(zip(fields[start::2], fields[start + 1::2]))
    if fields[0] == "STOP":
        stop()
    elif fields[0] == "SAY" and len(fields) >= 5:
        if fields[1] == "1":
            stop()
        said.put((generation, int(fields[2]), int(fields[3]), fields[4], pairs(5)))
    elif fields[0] == "RENDER" and len(fields) >= 3:
        renders.put((int(fields[1]), fields[2], pairs(3)))


threading.Thread(target=speak, daemon=True).start()
threading.Thread(target=render, daemon=True).start()
try:
    os.mkfifo(FIFO, 0o600)
    # A FIFO never written to does not read as closed, so it is opened without blocking and watched.
    r = os.open(FIFO, os.O_RDONLY | os.O_NONBLOCK)
    open(READY, "w").close()
    opened, pending = False, b""
    deadline = time.monotonic() + 15
    while opened or time.monotonic() < deadline:
        if not select.select([r], [], [], 1)[0]:
            continue
        data = os.read(r, 1 << 16)
        if not data:
            break
        opened = True
        *lines, pending = (pending + data).split(b"\n")
        for line in lines:
            handle(line.decode("utf-8", "replace").rstrip("\r"))
finally:
    for child in children:
        child.kill()
    for name in (READY, STATUS, FIFO):
        if os.path.exists(name):
            os.unlink(name)
