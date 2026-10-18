import asyncio
import os
import re

import decky

# Decky's bundled libraries break system tools such as curl, so clear them for children.
ENV = {**os.environ, "LD_LIBRARY_PATH": ""}
EMBER = os.path.join(decky.HOME, "Ember")


async def run(*args):
    p = await asyncio.create_subprocess_exec(
        *args, env=ENV, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    out = (await p.communicate())[0].decode(errors="replace")
    return p.returncode, out


class Plugin:
    async def status(self):
        installed = ""
        try:
            with open(os.path.join(EMBER, "BUILD_INFO.txt"), encoding="utf-8-sig") as f:
                installed = next((l.split(":", 1)[1].strip() for l in f if l.startswith("Release:")), "")
        except OSError:
            pass
        _, head = await run("curl", "-fsSI", "--max-time", "15", "https://github.com/Confetti3/SF4-Ember-Netplay/releases/latest")
        latest = re.search(r"(?im)^location:.*/tag/v?(\S+)", head)
        return {
            "installed": installed.removeprefix("v"),
            "latest": latest.group(1) if latest else "",
            "option": os.path.join(EMBER, "steam-launch.sh") + " %command%",
        }

    # Runs as the deck user: plugin.json has no "root" flag.
    async def install(self):
        code, out = await run("bash", os.path.join(decky.DECKY_PLUGIN_DIR, "steamdeck-install.sh"), "--no-steam")
        decky.logger.info(out)
        lines = out.strip().splitlines()
        return {"ok": code == 0, "message": lines[-1] if lines else ""}
