#!/usr/bin/env python3
# Ember's voice on Linux and the Steam Deck (src/platform/SpeechLinux.hxx).
# Ember starts it on the host once a session with the FIFO to read, and talks
# to it in lines of tab-separated fields:
#   SAY <interrupt 0|1> <rate -10..10> <volume 0..100> <language tag> (<key> <text>)...
#       where the key "wav" plays the WAV file named by the text instead
#   RENDER <rate> <language tag> (<key> <text>)...
#   STOP
#   PACK <update|remove> <language tag>
# It speaks from the language's voice pack, every line rendered ahead with one voice, where it
# has one; what a pack lacks (names, chat) and languages without a pack it speaks
# with a Piper voice for the language, downloaded once into
# ~/.cache/ember-speech and checked against its pinned SHA-256, and until then
# (or for a language without a voice) through speech-dispatcher. Every clip
# Piper makes is kept under Ember's key for it, so a part said once is ready at
# once; RENDER makes clips ahead at low priority. Stopping only ever stops
# Ember's own speech. It tells Ember how it speaks in <fifo>.status: a first line
# system, downloading <percent>, natural or failed, and a second line
# "pack <state> <bytes>" (status_text); and that it reads the FIFO by making
# <fifo>.ready. It leaves when Ember closes the FIFO, and after 15 seconds when
# Ember never writes.
import hashlib, json, os, queue, select, shutil, signal, socket, subprocess, sys, tarfile, threading, time, urllib.request, zipfile

STATUS = None
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "ember-speech")
PIPER_DIR = os.path.join(CACHE, "piper")
PIPER = ("https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz", 26460462,
         "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992")
VOICE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/c10ece1aade47bb51c153c893d14e5bf8e5b7117/"
# Ember's language tag to a Piper voice: its path, model size, model and config SHA-256.
# Only voices whose recordings are public domain, CC0 or CC BY (docs/design/ACCESSIBILITY_SPEC.md).
# Japanese, Korean and Chinese have none this Piper can speak under those terms, and stay on
# speech-dispatcher.
VOICES = {
    "en": ("en/en_US/ljspeech/medium/en_US-ljspeech-medium", 63531379, "6f52a751e2349abe7a76735eb09dc1875298c77ea2342ffd2fef79ff81b87f22", "41394a62f6aac38235193a755f81b07c24bf4aa72b17d2d6737cfb7c053799dc"),
    "de": ("de/de_DE/thorsten/medium/de_DE-thorsten-medium", 63201294, "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819", "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85"),
    "fr": ("fr/fr_FR/siwis/medium/fr_FR-siwis-medium", 63201294, "641d1ab097da2b81128c076810edb052b385decc8be3381814802a64a73baf99", "39479916c2db192b5ac9764daddd0c744d83e023ad890c6976c0633ae4df8959"),
    "es-ES": ("es/es_ES/davefx/medium/es_ES-davefx-medium", 63201294, "6658b03b1a6c316ee4c265a9896abc1393353c2d9e1bca7d66c2c442e222a917", "0e0dda87c732f6f38771ff274a6380d9252f327dca77aa2963d5fbdf9ec54842"),
    "es-419": ("es/es_MX/ald/medium/es_MX-ald-medium", 63201294, "019b3803293c93e34a206dd2e53a3889209a514e786fd7144f7b70196c579b63", "5a71498158e04afc8099bfd019c7e87c68eb9d042505a2b1a87e5c1ac2b1a61d"),
    "it": ("it/it_IT/paola/medium/it_IT-paola-medium", 63511038, "6fc918b5a0ea6137382833dddfa567bffbe6a5060c02043c87192ee59c04210c", "aea19c0a7fce29fbc359b93f10e7902854401e4c95ae2ea328ae516b15d296cf"),
    "nl": ("nl/nl_BE/nathalie/medium/nl_BE-nathalie-medium", 63201294, "49cf48023861f9fd42e13a8632f068fee67d1ce244a6ee38f29595afbf0a6be4", "4704af2736022e910a3f32672480d5530dd39da5c2bcc079f315f604166ff0de"),
    "pl": ("pl/pl_PL/gosia/medium/pl_PL-gosia-medium", 63201294, "38f66464240ed74f186e6b7dc13c6e3b22e023426299f25c2b3cc9dfa9373fbc", "1aefb31a9d53ffe44a8163ff73ec833acb7a6253848f6bb0403d8a66f9c7510d"),
    "pt-BR": ("pt/pt_BR/faber/medium/pt_BR-faber-medium", 63201294, "858555e3a064209c57088fe6bd70c4c3dc54d03eaa00c45d5ecaf43a33f95aa7", "7e694de195ae3fc36dd732c445eb04fb49b649854893cb5506b978f0d50a1d6f"),
    "ru": ("ru/ru_RU/denis/medium/ru_RU-denis-medium", 63201294, "15fab56e11a097858ee115545d0f697fc2a316c41a291a5362349fb870411b0a", "831c860dac0b5073eaa81610a0a638ec23d90a6cf8e5f871b4485c2cec3767c8"),
    "cs": ("cs/cs_CZ/jirka/medium/cs_CZ-jirka-medium", 63201294, "cbd5c900acacc8e8cbecd64347abb8de39c00a9d3104bed06fee92e4f319efc8", "fb38b1799b7354808227c065efa97b1ffa2b0cde59505babb56a36d35af9c637"),
}


# Ember's language tag to its voice pack: every line speech can say, rendered once with one voice
# (docs/design/ACCESSIBILITY_SPEC.md, "Pre-rendered voice packs"): version, url, size, SHA-256. A pack is
# downloaded only while speech is on, the first time the language is spoken, and replaced when this lists
# a newer version; one already in packs/<tag> is used whether or not it is listed here.
PACKS_URL = "https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/download/voice-packs/"
PACKS = {
    "en": (1, PACKS_URL + "ember-voice-en-1.zip", 18642574, "b602502e7c8c19e871622e9fb905294c4314b2dbecaa1cdd336879bb9f0e8461"),
}
PACKS_DIR = os.path.join(CACHE, "packs")


def pack_key(language, text):
    """Ember's ClipKey(language, 0, text) (SpeechLinux.hxx): a pack's clips are named by it."""
    h = 14695981039346656037
    for b in (language + "\0" + "0" + "\0" + text).encode("utf-8"):
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def pack_version(folder):
    """The version in an installed pack's manifest, 0 when there is none."""
    try:
        return int(json.load(open(os.path.join(folder, "manifest.json"), encoding="utf-8")).get("version", 0))
    except (OSError, ValueError):
        return 0


def pack_state(language):
    """removed (the player removed it), current, outdated (an older version than listed), missing
    (listed, not downloaded) or none (not listed, not installed)."""
    folder = os.path.join(PACKS_DIR, language)
    if os.path.exists(folder + ".removed"):
        return "removed"
    installed, listed = pack_version(folder), PACKS.get(language, (0,))[0]
    if installed:
        return "outdated" if listed > installed else "current"
    return "missing" if listed else "none"


def pack(language, force=False):
    """The language's pack folder when one is installed (an outdated one too, while its replacement
    downloads), starting a download when it is missing or outdated, or when forced."""
    folder, state = os.path.join(PACKS_DIR, language), pack_state(language)
    if state == "removed" and not force:
        return None
    if language in PACKS and (force or state in ("missing", "outdated")) and language not in downloading:
        downloading.add(language)
        threading.Thread(target=download_pack, args=(language,), daemon=True).start()
    return folder if pack_version(folder) else None


def install_pack(archive, folder):
    """Unpacks archive as folder, replacing an older pack there whole: the new one is unpacked beside it
    first, so a failure leaves the old one, and the old one is deleted after."""
    os.makedirs(os.path.dirname(folder), exist_ok=True)
    for leftover in (folder + ".part", folder + ".old"):
        shutil.rmtree(leftover, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            # Only flat clip and manifest names: nothing in a pack may write elsewhere.
            if os.path.basename(name) == name and (name.endswith(".opus") or name == "manifest.json"):
                z.extract(name, folder + ".part")
    if not pack_version(folder + ".part"):
        raise ValueError("the pack has no manifest")
    if os.path.exists(folder):
        os.replace(folder, folder + ".old")
    os.replace(folder + ".part", folder)
    shutil.rmtree(folder + ".old", ignore_errors=True)
    if os.path.exists(folder + ".removed"):
        os.unlink(folder + ".removed")


def remove_pack(language):
    folder = os.path.join(PACKS_DIR, language)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(PACKS_DIR, exist_ok=True)
    open(folder + ".removed", "w").close()
    status(None)


def download_pack(language):
    version, url, size, sha256 = PACKS[language]
    folder = os.path.join(PACKS_DIR, language)
    archive = "%s-%d.zip" % (folder, version)
    done = [0]
    def progress(count):
        done[0] += count
        status("downloading %d" % min(99, done[0] * 100 // size))
    status("downloading 0")
    try:
        os.makedirs(PACKS_DIR, exist_ok=True)
        fetch(url, archive, size, sha256, progress, resume=True)
        install_pack(archive, folder)
        os.unlink(archive)
        status("natural")
    except Exception as error:
        print("Ember speech: no voice pack download:", error, file=sys.stderr)
        status("failed")
    finally:
        downloading.discard(language)


voice_status, status_language, status_lock = "system", "", threading.Lock()


def status_text(voice_line, language):
    """What the status file says: how the relay speaks, then the language's pack and its size."""
    entry = PACKS.get(language)
    size = entry[2] if entry else 0
    return "%s\npack %s %d\n" % (voice_line, pack_state(language) if language else "none", size)


def status(text):
    """Records how the relay speaks (None keeps it) and writes the status file, when there is one."""
    global voice_status
    # The speaking thread and a download write it; one at a time, or one's rename finds no file.
    with status_lock:
        voice_status = text or voice_status
        if STATUS:
            with open(STATUS + ".tmp", "w") as f:
                f.write(status_text(voice_status, status_language))
            os.replace(STATUS + ".tmp", STATUS)


def model(language):
    """The voice's model file when it is downloaded, else None."""
    voice = VOICES.get(language)
    path = voice and os.path.join(CACHE, "voices", os.path.basename(voice[0]) + ".onnx")
    ok = path and os.path.exists(path) and os.path.exists(path + ".json") and os.path.exists(os.path.join(PIPER_DIR, "piper"))
    return path if ok else None


def fetch(url, path, size, sha256, progress, resume=False):
    """Downloads url to path through a .part file, checking its SHA-256. With resume, a .part left by
    a broken download continues where it stopped when the server allows; else it starts over.
    Returns the bytes that were already there."""
    part, digest, have = path + ".part", hashlib.sha256(), 0
    request = urllib.request.Request(url)
    if resume and os.path.exists(part) and url.startswith("http"):
        have = os.path.getsize(part)
        request.add_header("Range", "bytes=%d-" % have)
    with urllib.request.urlopen(request, timeout=30) as response:
        if have and response.status != 206:
            have = 0
        progress(have)
        if have:
            with open(part, "rb") as old:
                for chunk in iter(lambda: old.read(1 << 16), b""):
                    digest.update(chunk)
        with open(part, "ab" if have else "wb") as out:
            for chunk in iter(lambda: response.read(1 << 16), b""):
                digest.update(chunk)
                out.write(chunk)
                progress(len(chunk))
    if digest.hexdigest() != sha256:
        os.unlink(part)
        raise ValueError("SHA-256 mismatch for " + url)
    os.replace(part, path)
    return have


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


said, renders = queue.Queue(), queue.Queue()
lock = threading.Lock()
generation, player, broken, downloading, dispatcher = 0, None, set(), set(), None
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
    """The Piper model to speak the language with now, starting its download the first time;
    with a voice pack, only what the pack lacks needs it."""
    path = None if language in broken else model(language)
    if path:
        status("natural")
    elif language in PACKS:
        pass
    elif language in VOICES and language not in broken and language not in downloading:
        downloading.add(language)
        threading.Thread(target=download, args=(language,), daemon=True).start()
    elif language not in VOICES:
        status("system")
    return path


def speak():
    global player, status_language
    piper = None
    while True:
        heard, rate, volume, language, parts = said.get()
        status_language = language
        folder, path = pack(language), voice(language)
        if folder:
            status("natural")
        sounds = [text for key, text in parts if key == "wav"]
        parts = [(key, text) for key, text in parts if key != "wav"]
        # A part the pack has plays from it; the rest Piper makes, or with neither the system voice says it all.
        packed = {text: os.path.join(folder, pack_key(language, text) + ".opus") for _, text in parts} if folder else {}
        packed = {text: clip for text, clip in packed.items() if os.path.exists(clip)}
        if not path and not (parts and len(packed) == len(parts)):
            text = ", ".join(text for _, text in parts)
            dispatcher.sendall(("SET SELF RATE %d\r\nSET SELF VOLUME %d\r\nSET SELF LANGUAGE %s\r\nSPEAK\r\n%s%s\r\n.\r\n" % (
                rate * 10, volume * 2 - 100, language.split("-")[0], "." if text.startswith(".") else "", text)).encode())
            continue
        parts += [("wav", sound) for sound in sounds]
        if path and (not piper or piper.key != (path, rate)):
            piper = Piper(path, rate, False)
        for key, text in parts:
            clip = text if key == "wav" else packed.get(text) or clip_path(path, key)
            if heard != generation:
                break
            if key != "wav" and not os.path.exists(clip) and not piper.make(text, clip):
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
        if not path or os.path.exists(os.path.join(CACHE, "packs", language, "manifest.json")):
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
    elif fields[0] == "PACK" and len(fields) == 3:
        if fields[1] == "remove":
            remove_pack(fields[2])
        elif fields[1] == "update":
            pack(fields[2], force=True)


def main(fifo):
    global STATUS, dispatcher
    ready, STATUS = fifo + ".ready", fifo + ".status"
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    dispatcher = connect_dispatcher()
    threading.Thread(target=speak, daemon=True).start()
    threading.Thread(target=render, daemon=True).start()
    try:
        os.mkfifo(fifo, 0o600)
        # A FIFO never written to does not read as closed, so it is opened without blocking and watched.
        r = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        open(ready, "w").close()
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
        for name in (ready, STATUS, fifo):
            if os.path.exists(name):
                os.unlink(name)


if __name__ == "__main__":
    main(sys.argv[1])
