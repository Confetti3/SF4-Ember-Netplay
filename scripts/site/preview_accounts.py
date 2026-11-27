"""Made-up answers for the points boards and the signed-in pages, for preview.py
(docs/design/SITE_PLAYER_CARDS.md): GET /points.json, GET /account/me.json and POST /account/card.json,
and /account/sign-in and /account/sign-out, which switch the preview's session in memory.

The world is small and fixed (preview-cards.json): the named players with their own numbers, and a few
dozen invented handles, each with points that go down the list and two fighters. A game is 1 point and
a third of an invented player's points are win bonuses, so their points are their games plus their wins.
The boards are worked out from everyone, so a player's places are where the boards list them. Every
state a page shows has a session state here (`STATES`).
"""
import json
from dataclasses import dataclass, replace
from pathlib import Path
from urllib.parse import parse_qs

SAMPLES = Path(__file__).with_name("preview-cards.json")
FIGHTERS = 44
MAX_BOARD_LINES = 500
# The session states: signed out, signed in to a Discord account no Ember ID has connected, and signed in,
# with a settled class, before any rated game, still placing, faint after a break, or no longer taking
# part in ranking (rating kept, every board left).
SIGNED_IN = ("signed_in", "unrated", "placing", "faint", "not_taking_part")
STATES = ("signed_out", "no_account") + SIGNED_IN
# What the signed-in player's class is in each signed-in state: as the samples have it (None), placing,
# faint, or none shown because they left the boards.
BADGES = {"signed_in": None, "unrated": (0, False), "placing": (0, False), "faint": "faint", "not_taking_part": "out"}


@dataclass(frozen=True)
class Person:
    name: str
    country: str | None
    # (band, settled) as the badge says it, or None for no class shown (not taking part).
    badge: tuple | None
    main: int | None
    since: int
    token: str
    points: int
    # {fighter: (points, games, wins)}
    fighters: dict
    # games, wins, best_streak, peak_class, rival, opponents as an account has them.
    awards: dict

    @property
    def listed(self):
        return self.badge is not None

    @property
    def board_band(self):
        """The class the boards show: none while placing or faint."""
        return self.badge[0] if self.badge and self.badge[1] and self.badge[0] else None


def counted(points):
    """An invented count: a third of the points are win bonuses, the rest games."""
    wins = points // 3
    return points, points - wins, wins


def named(entry):
    return Person(name=entry["name"], country=entry.get("country"), badge=(entry["band"], True) if "band" in entry else (0, False),
                  main=entry.get("main"), since=entry["since"], token=entry["token"], points=entry["points"],
                  fighters={f: (p, g, w) for f, p, g, w in entry["fighters"]}, awards=entry["awards"])


def invented(at, handle, data):
    """The at-th invented player: fewer points down the list, a main and a second fighter, a class that
    is placing every eleventh and faint every seventh."""
    points, games, wins = counted(data["top"] - data["step"] * at)
    main, second = (at * 7) % 42, (at * 7 + 5) % 42
    fighters = {main: counted(points * 6 // 10), second: counted(points * 3 // 10)}
    band = at % 11
    return Person(name=handle, country=data["countries"][at % len(data["countries"])], badge=(band, band > 0 and at % 7 != 0),
                  main=main, since=data["listed_at"] - (at + 1) * 7 * 86400, token=f"c1.preview-{at:03d}", points=points,
                  fighters=fighters, awards={"games": games, "wins": wins, "best_streak": 1 + at % 9, "peak_class": band})


def frame(points, starts):
    """The tier of a points count with where it begins and where the next begins (None at the top)."""
    tier = max(at for at, start in enumerate(starts) if points >= start)
    return tier, starts[tier], starts[tier + 1] if tier + 1 < len(starts) else None


class World:
    """Everyone as one session state has the signed-in player: their badge, and whether they are listed."""

    def __init__(self, data, state):
        self.data = data
        people = [named(entry) for entry in data["players"]] + [invented(at, handle, data) for at, handle in enumerate(data["handles"])]
        badge = BADGES.get(state)
        you = next(person for person in people if person.name == data["you"])
        mine = {None: you, "out": replace(you, badge=None), "faint": replace(you, badge=(you.badge[0], False))}.get(badge, replace(you, badge=badge))
        self.people = [mine if person is you else person for person in people]
        self.you = mine

    def board(self, fighter):
        """[(place, person, points)] of every listed player on the account board (None) or a fighter's."""
        entries = sorted(((person, person.points if fighter is None else person.fighters[fighter][0])
                          for person in self.people if person.listed and (fighter is None or fighter in person.fighters)),
                         key=lambda entry: (-entry[1], entry[0].name))
        lines = []
        for at, (person, points) in enumerate(entries):
            place = lines[-1][0] if lines and lines[-1][2] == points else at + 1
            lines.append((place, person, points))
        return lines

    def places(self, person):
        def place(fighter):
            return next((place for place, listed, _ in self.board(fighter) if listed is person), None)
        account = place(None)
        fighters = [[fighter, place(fighter)] for fighter in sorted(person.fighters) if place(fighter)]
        return ({"account": account} if account else {}) | {"fighters": fighters}

    def tier(self, points, kind):
        return frame(points, self.data["thresholds"][kind])

    def badge(self, person):
        return {"badge": {"band": person.badge[0], "settled": person.badge[1]}} if person.badge else {}

    def card(self, person):
        """The PlayerCard of `person`, as the signed-in player sees it."""
        awards = {name: value for name, value in person.awards.items()
                  if name in ("games", "wins") or (value and not (name == "peak_class" and not person.listed))}
        card = {"account": {"points": person.points, "tier": self.tier(person.points, "account")[0]},
                "fighters": [{"fighter": fighter, "points": points, "tier": self.tier(points, "fighter")[0]}
                             for fighter, (points, _, _) in sorted(person.fighters.items())],
                "relation": "none", "awards": awards, "name": person.name, "since": person.since}
        card |= ({"country": person.country} if person.country else {}) | ({"main": person.main} if person.main is not None else {})
        versus = self.data["versus"].get(person.name) if person is not self.you else None
        return card | self.badge(person) | ({"versus": versus} if versus else {})

    def own(self):
        """The signed-in player's OwnLevels, counted."""
        def framed(points, kind):
            tier, floor, next_at = self.tier(points, kind)
            return {"tier": tier, "floor": floor} | ({"next": next_at} if next_at is not None else {})
        you = self.you
        account = {"points": you.points} | {name: value for name, value in you.awards.items() if value} | framed(you.points, "account")
        fighters = [{"fighter": fighter, "points": points, "games": games, "wins": wins} | framed(points, "fighter") | {"since": you.since}
                    for fighter, (points, games, wins) in sorted(you.fighters.items())]
        return {"points": "counted", "account": account | {"since": you.since}, "fighters": fighters}

    def hit(self, person):
        hit = {"card": person.token, "name": person.name, "since": person.since}
        hit |= ({"country": person.country} if person.country else {}) | ({"main": person.main} if person.main is not None else {})
        return hit | self.badge(person)


def error(code, message, **details):
    return {"error": {"code": code, "message": message} | ({"details": details} if details else {})}


class Accounts:
    """The account routes of one preview server, whose session starts as `state`."""

    def __init__(self, state, path=SAMPLES):
        self.data = json.loads(path.read_text(encoding="utf-8"))
        self.state = state
        # What signing in signs in as: the state asked for, or a settled class.
        self.signed_in_as = state if state in SIGNED_IN else "signed_in"

    def world(self):
        return World(self.data, self.state if self.state in SIGNED_IN else "signed_in")

    def board(self, query):
        # As nginx passes it on (ember-short-locations.conf): only `fighter`, empty for the account board,
        # 0 to 43 without a leading zero for a fighter's, refused otherwise.
        fighter = parse_qs(query, keep_blank_values=True).get("fighter", [""])[0]
        if fighter and not (fighter.isdigit() and str(int(fighter)) == fighter and int(fighter) < FIGHTERS):
            return 400, error("invalid", "fighter must be 0 to 43, or left out for the account board")
        fighter = int(fighter) if fighter else None
        world = self.world()
        lines = world.board(fighter)
        players = [{"place": place, "name": person.name} | ({"country": person.country} if person.country else {})
                   | ({"band": person.board_band} if person.board_band else {})
                   | {"points": points, "tier": world.tier(points, "account" if fighter is None else "fighter")[0], "card": person.token}
                   for place, person, points in lines[:MAX_BOARD_LINES]]
        body = {"listed_at": self.data["listed_at"]} | ({"fighter": fighter} if fighter is not None else {})
        return 200, body | {"total": len(lines), "players": players}

    def me(self):
        if self.state not in SIGNED_IN:
            return 200, {"state": self.state}
        world = self.world()
        rating = self.data["ratings"].get(self.state)
        body = {"state": "signed_in", "card": world.card(world.you), "points": world.own(), "places": world.places(world.you),
                "link": world.you.token}
        return 200, body | ({"rating": rating} if rating else {}) | {"rules": self.data["rules"]}

    def card(self, raw):
        if self.state == "signed_out":
            return 401, error("unauthenticated", "Sign in with Discord to open player cards.")
        if self.state == "no_account":
            return 403, error("forbidden", "Connect this Discord account in Ember to open player cards.", reason="no_ember_id")
        try:
            asked = json.loads(raw)
        except ValueError:
            return 400, error("invalid", "the body is not JSON")
        if not isinstance(asked, dict) or len(asked) != 1 or not isinstance(next(iter(asked.values())), str):
            return 400, error("invalid", "ask for one card or one name")
        world = self.world()
        others = [person for person in world.people if person is not world.you]
        if "card" in asked:
            found = [person for person in others if person.token == asked["card"]]
        elif "name" in asked and 2 <= len(asked["name"].strip()) <= 32:
            found = [person for person in others if person.name.lower() == asked["name"].strip().lower()]
        else:
            return 400, error("invalid", "ask for one card or one name")
        if len(found) > 1:
            return 200, {"players": [world.hit(person) for person in found]}
        if not found:
            return 200, {"card": None}
        return 200, {"card": world.card(found[0]), "places": world.places(found[0])}

    @staticmethod
    def own_page(headers):
        """Whether a request came from the site's own page, as the bridge takes a sign-out: Sec-Fetch-Site
        says same-origin, or, from a browser without it, Origin is the site's own."""
        fetched = headers.get("Sec-Fetch-Site")
        if fetched is not None:
            return fetched == "same-origin"
        return headers.get("Origin") == "http://" + (headers.get("Host") or "")

    def get(self, path, query):
        """(status, body, content type, other headers) for a GET of these routes, or None for another path."""
        if path == "/points.json":
            return self.answer(*self.board(query))
        if path == "/account/me.json":
            return self.answer(*self.me())
        if path == "/account/sign-in":
            self.state = self.signed_in_as
            return 302, b"", "text/plain", {"Location": "/me"}
        return None

    def post(self, path, body, headers):
        if path == "/account/sign-out":
            if not self.own_page(headers):
                return self.answer(403, error("forbidden", "Sign out from the site's own page."))
            self.state = "signed_out"
            return 204, b"", "text/plain", {}
        if path == "/account/card.json":
            return self.answer(*self.card(body))
        return None

    @staticmethod
    def answer(status, body):
        return status, json.dumps(body).encode(), "application/json", {}
