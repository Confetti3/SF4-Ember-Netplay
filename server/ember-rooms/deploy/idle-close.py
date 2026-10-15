#!/usr/bin/env python3
"""Closes public rooms that a lone host has left sitting.

A room is quiet while it has at most one member and no table playing. Once a
room has been quiet for --idle-minutes (15) in a row, this asks the supervisor
to close it (DELETE /rooms/<id>), which closes it the same way the bridge
does. A second member or a match starting resets the clock. Empty rooms are
not this script's business: the supervisor closes them after
empty_close_secs.

Each run polls GET /rooms once and keeps, per room, when it was first seen
quiet, so the timer that runs it (ember-rooms-idle.timer, every 30 seconds)
is what measures the time. A member who comes and goes, or a match that
starts and ends, between two polls is not seen. If the last poll is more than
MAX_GAP_SECS old (the timer was stopped, the box rebooted, or the supervisor
did not answer), nobody watched the rooms in between, so every clock starts
again from this poll: a room is only closed after 15 minutes it was seen
quiet the whole time.

Run by ember-rooms-idle.service. By hand, to see what it would close:
    sudo python3 ~/ember-rooms/idle-close.py --dry-run
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

ETC = "/etc/ember-rooms"
STATE_DIR = "/var/lib/ember-rooms-idle"
DEFAULT_BIND = "127.0.0.1:47830"
# Four missed 30-second runs.
MAX_GAP_SECS = 120


def credential(name):
    """The service gets the config and the secret as systemd credentials;
    run by hand it reads them from /etc/ember-rooms."""
    directory = os.environ.get("CREDENTIALS_DIRECTORY")
    return os.path.join(directory, name) if directory else os.path.join(ETC, name)


def request(method, url, secret):
    req = urllib.request.Request(
        url, method=method, headers={"Authorization": "Bearer " + secret}
    )
    with urllib.request.urlopen(req, timeout=5) as response:
        return response.status, response.read()


def describe(room):
    details = room.get("details") or {}
    name = str(details.get("name", "")).replace("\n", " ")[:60]
    host = str(details.get("host_name", "")).replace("\n", " ")[:40]
    return "%s (%r, host %r)" % (room["room_id"], name, host)


def load_state(path, now):
    """When each room was first seen quiet, or nothing when the last poll
    was not recent enough to vouch for the time since."""
    try:
        with open(path) as f:
            state = json.load(f)
        polled_at = float(state["polled_at"])
        quiet_since = {k: float(v) for k, v in state["quiet_since"].items()}
    except (OSError, ValueError, KeyError, AttributeError, TypeError):
        return {}
    if not 0 <= now - polled_at <= MAX_GAP_SECS:
        return {}
    return quiet_since


def save_state(path, now, quiet_since):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"polled_at": now, "quiet_since": quiet_since}, f)
    os.replace(tmp, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--idle-minutes", type=float, default=15)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the quiet rooms and what would close; change nothing",
    )
    args = parser.parse_args()
    if args.idle_minutes <= 0:
        parser.error("--idle-minutes must be above 0")

    with open(credential("config.json")) as f:
        bind = json.load(f).get("bind", DEFAULT_BIND)
    with open(credential("supervisor.secret")) as f:
        secret = "".join(f.read().split())
    base = "http://" + bind

    try:
        _, body = request("GET", base + "/rooms", secret)
        rooms = json.loads(body)
    except urllib.error.HTTPError as e:
        # A refused secret is a broken install, so the unit should fail.
        print("idle-close: GET /rooms answered HTTP %d" % e.code, file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError, ValueError) as e:
        # The supervisor restarting or draining is not an error worth
        # failing the unit for; the next run tries again. The kept state
        # stays as it was.
        print("idle-close: GET /rooms failed: %s" % e, file=sys.stderr)
        return 0

    state_path = os.path.join(
        os.environ.get("STATE_DIRECTORY", STATE_DIR), "state.json"
    )
    now = time.time()
    previous = load_state(state_path, now)
    limit = args.idle_minutes * 60
    quiet_since = {}
    to_close = []
    for room in rooms:
        room_id = room["room_id"]
        if room.get("members", 0) > 1 or room.get("tables_playing", 0) > 0:
            continue
        since = previous.get(room_id, now)
        quiet_since[room_id] = since
        quiet = now - since
        if args.dry_run:
            print(
                "quiet %4.1f min, %d member(s): %s"
                % (quiet / 60, room.get("members", 0), describe(room))
            )
        if quiet >= limit:
            to_close.append((room, quiet))

    if args.dry_run:
        print(
            "%d room(s) listed, %d quiet, %d past %g min (dry run: nothing closed)"
            % (len(rooms), len(quiet_since), len(to_close), args.idle_minutes)
        )
        return 0

    for room, quiet in to_close:
        room_id = room["room_id"]
        try:
            request("DELETE", base + "/rooms/" + room_id, secret)
            print(
                "idle-close: closed %s: %d member(s), no match for %.0f min"
                % (describe(room), room.get("members", 0), quiet / 60)
            )
        except urllib.error.HTTPError as e:
            if e.code != 404:
                print("idle-close: close %s failed: HTTP %d" % (room_id, e.code), file=sys.stderr)
                continue
        except (urllib.error.URLError, OSError) as e:
            print("idle-close: close %s failed: %s" % (room_id, e), file=sys.stderr)
            continue
        quiet_since.pop(room_id, None)

    # Rooms no longer listed, or no longer quiet, drop out here.
    save_state(state_path, now, quiet_since)
    return 0


if __name__ == "__main__":
    sys.exit(main())
