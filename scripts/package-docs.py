"""Relocate package Markdown links and reject missing local targets.

Unshipped source references point at the exact source revision. Player support
guides are included in the inventory so they remain available offline.
"""
import argparse
import os
from pathlib import Path
import re
from urllib.parse import quote, unquote, urlsplit

LINK = re.compile(r'(!?\[[^\]\n]*\]\()(<[^>\n]+>|[^\s)]+)([^)\n]*\))')
REFERENCE = re.compile(r'^(\s*\[[^\]\n]+\]:\s*)(<[^>\n]+>|\S+)(.*)$', re.M)


def local_path(target):
    parsed = urlsplit(target.replace('\\', '/'))
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    return unquote(parsed.path)


def transform(text, callback):
    def replace(match):
        raw = match[2]
        bracketed = raw.startswith('<') and raw.endswith('>')
        value = callback(raw[1:-1] if bracketed else raw)
        return match[1] + ('<' + value + '>' if bracketed else value) + match[3]
    return REFERENCE.sub(replace, LINK.sub(replace, text))


def relocate(repo, package, revision, quick_start=None):
    if not re.fullmatch(r'[0-9a-fA-F]{40}', revision):
        raise ValueError('An exact 40-character source commit is required')
    mappings = {}
    for output in package.rglob('*.md'):
        relative = output.relative_to(package)
        if relative.as_posix() == 'START_HERE.md':
            source = quick_start or repo / 'docs/guides/USER_NETPLAY.md'
        elif relative.as_posix() == 'SECURITY.md':
            source = repo / '.github/SECURITY.md'
        elif (repo / relative).is_file():
            source = repo / relative
        elif relative.parts[0] == 'docs':
            candidates = list((repo / 'docs').glob('*/' + relative.name))
            if len(candidates) != 1:
                raise ValueError(f'Ambiguous or absent document source: {relative}')
            source = candidates[0]
        else:
            raise ValueError(f'Unknown document source: {relative}')
        mappings[output.resolve()] = source.resolve()
    # START_HERE is preferred when the same player guide also ships under docs/.
    destinations = {source: output for output, source in mappings.items()}
    start = (package / 'START_HERE.md').resolve()
    if start in mappings:
        destinations[mappings[start]] = start
    for output, source in mappings.items():
        def rewrite(target):
            path = local_path(target)
            if path is None:
                return target
            referenced = (source.parent / path).resolve()
            if not referenced.is_relative_to(repo) or not referenced.exists():
                raise ValueError(f'{source}: missing or outside-source link {target}')
            parsed = urlsplit(target.replace('\\', '/'))
            suffix = ('?' + parsed.query if parsed.query else '') + ('#' + parsed.fragment if parsed.fragment else '')
            destination = destinations.get(referenced, package / referenced.relative_to(repo))
            if destination.exists():
                return quote(Path(os.path.relpath(destination, output.parent)).as_posix(), safe='/.-_') + suffix
            return 'https://github.com/Confetti3/SF4-Ember-Netplay/blob/' + revision + '/' + quote(referenced.relative_to(repo).as_posix(), safe='/.-_') + suffix
        output.write_text(transform(source.read_text(encoding='utf-8-sig'), rewrite), encoding='utf-8', newline='\n')
    return check(package)


def check(package):
    errors = []
    for document in package.rglob('*.md'):
        def validate(target):
            path = local_path(target)
            if path is not None:
                resolved = (document.parent / path).resolve()
                if not resolved.is_relative_to(package) or not resolved.exists():
                    errors.append(f'{document.relative_to(package)}: {target}')
            return target
        transform(document.read_text(encoding='utf-8-sig'), validate)
    if errors:
        raise ValueError('Broken package links:\n' + '\n'.join(errors))
    return len(list(package.rglob('*.md')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--revision')
    parser.add_argument('--quick-start', type=Path)
    args = parser.parse_args()
    package = args.package.resolve()
    if not package.is_dir():
        parser.error('Package directory does not exist')
    count = relocate(args.repo.resolve(), package, args.revision or '', args.quick_start.resolve() if args.quick_start else None) if args.repo else check(package)
    print(f'Package documentation: {count} Markdown files, no broken local file links')
