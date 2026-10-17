"""Relocate copied package documents using their explicit source mapping."""
import argparse
import html
import json
import sys
from importlib.metadata import version
import os
from pathlib import Path
import re
from urllib.parse import quote, unquote, urlsplit

if sys.version_info < (3, 10):
    raise SystemExit("Package documentation requires Python 3.10 or newer.")

try:
    from marko import Markdown, block, inline
    from marko.helpers import MarkoExtension
except ImportError as error:
    raise SystemExit("Install scripts/requirements-docs.txt with the selected Python interpreter.") from error

if version("marko") != "2.2.4":
    raise SystemExit("Install the pinned parser from scripts/requirements-docs.txt.")


class LinkRefDef(block.LinkRefDef):
    override = True

    @classmethod
    def parse(cls, source):
        destination = source.context.linkref_info.link_dest
        node = super().parse(source)
        node.dest_span = (destination.start, destination.end)
        return node


def link_spans(text):
    """Use the parser's source spans; code and literal examples are not links."""
    root = Markdown(extensions=[MarkoExtension(elements=[LinkRefDef])]).parse(text)

    def walk(node):
        span = getattr(node, "dest_span", None)
        if span is not None:
            yield span
        children = getattr(node, "children", ())
        if not isinstance(children, str):
            for child in children:
                yield from walk(child)

    return sorted(set(walk(root)))


def transform(text, callback):
    edits = []
    previous = 0
    for start, end in link_spans(text):
        if start < previous or not 0 <= start <= end <= len(text):
            raise ValueError("Overlapping or invalid Markdown destination spans")
        raw = text[start:end]
        bracketed = raw.startswith("<") and raw.endswith(">")
        target = raw[1:-1] if bracketed else raw
        target = html.unescape(inline.Literal.strip_backslash(target))
        replacement = callback(target)
        # Preserve external destinations byte-for-byte, including their escapes.
        if replacement != target:
            edits.append((start, end, "<" + replacement + ">" if bracketed else replacement))
        previous = end
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return text


def local_path(target):
    parsed = urlsplit(target.replace("\\", "/"))
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    return unquote(parsed.path)


def markdown_files(package):
    return {p.resolve() for p in package.rglob("*") if p.is_file() and p.suffix.lower() == ".md"}


def relocate(repo, package, revision, records):
    repo, package = repo.resolve(), package.resolve()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", revision):
        raise ValueError("An exact 40-character source commit is required")
    mappings = {}
    outputs = set()
    destinations = {}
    preferred = set()
    for record in records:
        output = (package / record["destination"]).resolve()
        if not output.is_relative_to(package) or not output.is_file():
            raise ValueError("Missing or outside-package document: " + record["destination"])
        if output in outputs:
            raise ValueError("Duplicate document destination: " + record["destination"])
        source = Path(record["source"]).resolve()
        if not source.is_file():
            raise ValueError("Selected document source no longer exists: " + str(source))
        outputs.add(output)
        if output.suffix.lower() == ".md":
            mappings[output] = source
        for alias in {source, *(Path(p).resolve() for p in record.get("aliases", []))}:
            if record.get("preferred", False):
                if alias in preferred and destinations[alias] != output:
                    raise ValueError("Multiple preferred destinations for " + str(alias))
                destinations[alias] = output
                preferred.add(alias)
            elif alias not in destinations:
                destinations[alias] = output
    if set(mappings) != markdown_files(package):
        missing = markdown_files(package) - set(mappings)
        raise ValueError("Every packaged Markdown file needs an explicit mapping: " + str(sorted(map(str, missing))))

    for output, origin in mappings.items():
        def rewrite(target):
            path = local_path(target)
            if path is None:
                return target
            referenced = (origin.parent / path).resolve()
            parsed = urlsplit(target.replace("\\", "/"))
            suffix = ("?" + parsed.query if parsed.query else "") + ("#" + parsed.fragment if parsed.fragment else "")
            destination = destinations.get(referenced)
            if destination is None and referenced.is_relative_to(repo):
                candidate = (package / referenced.relative_to(repo)).resolve()
                if candidate.is_relative_to(package) and candidate.exists():
                    destination = candidate
            if destination is not None:
                return quote(Path(os.path.relpath(destination, output.parent)).as_posix(), safe="/.-_") + suffix
            if not referenced.is_relative_to(repo) or not referenced.exists():
                raise ValueError(f"{origin}: missing or outside-source link {target}")
            kind = "tree" if referenced.is_dir() else "blob"
            return f"https://github.com/Confetti3/SF4-Ember-Netplay/{kind}/{revision}/" + quote(referenced.relative_to(repo).as_posix(), safe="/.-_") + suffix

        # The package already contains the selected staged/custom document.
        # The mapping supplies link origins and aliases, never replacement prose.
        output.write_text(transform(output.read_text(encoding="utf-8-sig"), rewrite), encoding="utf-8", newline="\n")
    return check(package)


def check(package):
    package = package.resolve()
    errors = []
    documents = markdown_files(package)
    for document in sorted(documents):
        def validate(target):
            path = local_path(target)
            if path is not None:
                resolved = (document.parent / path).resolve()
                if not resolved.is_relative_to(package) or not resolved.exists():
                    errors.append(f"{document.relative_to(package)}: {target}")
            return target

        transform(document.read_text(encoding="utf-8-sig"), validate)
    if errors:
        raise ValueError("Broken package links:\n" + "\n".join(errors))
    return len(documents)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--mapping", type=Path)
    args = parser.parse_args()
    if not args.package.is_dir():
        parser.error("Package directory does not exist")
    if args.repo:
        if not args.mapping:
            parser.error("--mapping is required for relocation")
        records = json.loads(args.mapping.read_text(encoding="utf-8-sig"))
        count = relocate(args.repo, args.package, args.revision or "", records)
    else:
        count = check(args.package)
    print(f"Package documentation: {count} Markdown files, no broken local file links")
