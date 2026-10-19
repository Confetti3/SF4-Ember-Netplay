import asyncio
import os
import re

import decky

# Decky's bundled libraries break system tools such as curl, so clear them for children.
# The installer restarts Steam through the deck user's systemd, which needs its session.
RUN = f"/run/user/{os.getuid()}"
ENV = {**os.environ, "LD_LIBRARY_PATH": "", "XDG_RUNTIME_DIR": RUN, "DBUS_SESSION_BUS_ADDRESS": f"unix:path={RUN}/bus"}
STEAM = os.path.join(decky.HOME, ".local/share/Steam")


def ember():
    """C:\\Ember in the game's prefix, in whichever library holds the game, as the installer puts it."""
    libs = [STEAM]
    try:
        libs += re.findall(r'"path"\s+"([^"]+)"', open(os.path.join(STEAM, "steamapps/libraryfolders.vdf"), encoding="utf-8").read())
    except OSError:
        pass
    return next((e for e in (os.path.join(l, "steamapps/compatdata/45760/pfx/drive_c/Ember") for l in libs) if os.path.isdir(e)), "")


async def run(*args):
    p = await asyncio.create_subprocess_exec(
        *args, env=ENV, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    out = (await p.communicate())[0].decode(errors="replace")
    return p.returncode, out


class Plugin:
    async def status(self):
        installed = ""
        try:
            with open(os.path.join(ember(), "BUILD_INFO.txt"), encoding="utf-8-sig") as f:
                installed = next((l.split(":", 1)[1].strip() for l in f if l.startswith("Release:")), "")
        except OSError:
            pass
        _, head = await run("curl", "-fsSI", "--max-time", "15", "https://github.com/Confetti3/SF4-Ember-Netplay/releases/latest")
        latest = re.search(r"(?im)^location:.*/tag/v?(\S+)", head)
        return {
            "installed": installed.removeprefix("v"),
            "latest": latest.group(1) if latest else "",
        }

    # Runs as the deck user: plugin.json has no "root" flag. Steam restarts
    # during the install; this backend is Decky's own service and keeps running.
    async def install(self):
        code, out = await run("bash", os.path.join(decky.DECKY_PLUGIN_DIR, "steamdeck-install.sh"))
        decky.logger.info(out)
        lines = out.strip().splitlines()
        return {"ok": code == 0, "message": lines[-1] if lines else ""}
