#!/usr/bin/env python3
"""Serve the site on this PC to look at a change: python3 scripts/site/preview.py --wiki-frame-data [--port 18733]

Builds both folders into build/preview (make.py, with the two hosts' links pointed at this server) and
serves them the way nginx does on embernetplay.link: the same paths, and the same Content-Security-Policy
on each page, so a style or script the live site would refuse is refused here too.

  /                    the main site (build/preview/site)
  /replays/            the replay archive's page (build/preview/archive-site)
  /rooms.json, ...     the live site's public lists, fetched from embernetplay.link
  /replays/v1/archive  preview-archive.json, made-up games with every kind of result; --live-archive
                       fetches the live archive's listing instead
"""
import argparse
import mimetypes
import re
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import make
from shell import ROOT, Host

LIVE = "https://embernetplay.link"
LIVE_ARCHIVE = "https://archive.embernetplay.link"
LISTS = {"/rooms.json", "/rooms-sfv.json", "/rankings.json", "/rankings-sfv.json"}
SAMPLE_ARCHIVE = Path(__file__).with_name("preview-archive.json")
# As server/ember-short/deploy/nginx/ember-short-locations.conf and
# server/ember/archive-site/deploy/nginx/archive-locations.conf.in send them.
SITE_POLICY = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; "
               "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
ARCHIVE_POLICY = "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
MAIN = Host(site="", assets="/assets", replays="/replays/")
ARCHIVE = Host(site="", assets="assets", replays="./", favicon=False, bundle="archive")
PAGES = {"/": "index.html", "/rooms": "rooms.html", "/rankings": "rankings.html", "/docs": "docs/index.html",
         "/frame-data": "frame-data/index.html"}
OPENS = re.compile(r"/([jmrv](/.*)?|start/?)$")
GUIDE = re.compile(r"/(docs|frame-data)/([a-z0-9-]+)$")
FILE = re.compile(r"/(assets|docs/files)/[\w./-]+$|/docs/search\.json$|/favicon\.ico$")


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "ember-site-preview"})
    with urllib.request.urlopen(request, timeout=15) as response:
        return response.read()


class Preview(BaseHTTPRequestHandler):
    site: Path
    archive: Path
    live_archive: bool

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path in LISTS:
            return self.passed_on(LIVE + path)
        if path == "/replays/v1/archive":
            if self.live_archive:
                return self.passed_on(f"{LIVE_ARCHIVE}/v1/archive" + ("?" + query if query else ""))
            return self.send(200, SAMPLE_ARCHIVE.read_bytes(), "application/json")
        if path == "/replays/":
            return self.file(self.archive / "index.html", ARCHIVE_POLICY)
        if path.startswith("/replays/assets/") and ".." not in path:
            # The archive's host has its own page files and copies of the main site's images and fonts.
            name = path.removeprefix("/replays/assets/")
            own = self.archive / "assets" / name
            return self.file(own if own.is_file() else self.site / "assets" / name)
        if path in PAGES:
            return self.file(self.site / PAGES[path], SITE_POLICY)
        if OPENS.match(path):
            return self.file(self.site / "open.html", SITE_POLICY)
        guide = GUIDE.match(path)
        if guide:
            return self.file(self.site / guide.group(1) / f"{guide.group(2)}.html", SITE_POLICY)
        if FILE.match(path) and ".." not in path:
            return self.file(self.site / path.lstrip("/"))
        self.missing()

    def file(self, path, policy=None):
        if not path.is_file():
            return self.missing()
        kind = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        self.send(200, path.read_bytes(), kind + ("; charset=utf-8" if kind.startswith("text/") or kind.endswith("javascript") else ""), policy)

    def missing(self):
        self.send(404, (self.site / "404.html").read_bytes(), "text/html; charset=utf-8", SITE_POLICY)

    def passed_on(self, url):
        try:
            self.send(200, fetch(url), "application/json")
        except (urllib.error.URLError, TimeoutError) as error:
            self.send(502, str(error).encode(), "text/plain; charset=utf-8")

    def send(self, status, body, kind, policy=None):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if policy:
            self.send_header("Content-Security-Policy", policy)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        if args and str(args[1]) != "200":
            sys.stderr.write("%s %s\n" % (args[1], args[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=18733)
    data = parser.add_mutually_exclusive_group(required=True)
    data.add_argument("--frame-data", type=Path, help="a folder with the frame data pages")
    data.add_argument("--wiki-frame-data", action="store_true", help="take the frame data pages from the published wiki")
    parser.add_argument("--live-archive", action="store_true", help="list the live archive's games instead of the made-up ones")
    args = parser.parse_args()
    out = ROOT / "build/preview"
    folder = make.wiki_frame_data(out) if args.wiki_frame_data else args.frame_data
    make.build(ROOT, out, folder, MAIN, ARCHIVE)
    Preview.site, Preview.archive, Preview.live_archive = out / "site", out / "archive-site", args.live_archive
    print(f"http://localhost:{args.port}/  (the replay archive at /replays/)", flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.port), Preview).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
