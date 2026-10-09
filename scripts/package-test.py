"""Assemble an explicitly SDK-free private test ZIP from freshly tested binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from private_build_layout import publish_private_build


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args):
    environment = os.environ.copy()
    if str(args[0]).lower() == 'powershell.exe':
        # Windows PowerShell must discover its own modules when this packager
        # is launched from PowerShell 7 (which exports a different module path).
        for key in list(environment):
            if key.lower() == 'psmodulepath':
                del environment[key]
    subprocess.run([str(arg) for arg in args], check=True, env=environment)


def remove_verified_extraction(verify, build_root):
    if (verify.is_symlink() or not verify.name.endswith('-extracted') or
            verify.resolve().parent != build_root.resolve()):
        raise ValueError(f'Refusing to clean an extraction outside the build directory: {verify}')
    shutil.rmtree(verify)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--crt', type=Path, required=True)
    parser.add_argument('--label', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9._-]+', args.label):
        parser.error('Invalid label')
    repo = Path(__file__).resolve().parent.parent
    # Failed or in-progress candidates never appear beside the playable build.
    # Scratch is removed on both success and failure; published output is moved.
    with tempfile.TemporaryDirectory(prefix='private-package-', dir=repo / 'build') as scratch:
        assemble(args, repo, Path(scratch))


def assemble(args, repo, staging):
    (repo / 'dist').mkdir(exist_ok=True)
    dest = staging / ('sf4-ember-netplay-' + args.label)
    archive = dest.with_name(dest.name + '.zip')
    if dest.exists() or archive.exists():
        raise ValueError('Choose a fresh label; existing packages are never overwritten')
    dest.mkdir(parents=True)

    def copy(source, relative):
        output = dest / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, output)

    excluded = {'ember-discord.exe', 'discord_partner_sdk.dll', 'discord-build.json', 'notices/Discord-SDK.txt', 'docs/DISCORD.md'}
    generated = {'PackageInventory.inc', 'preflight.ps1', 'preflight.cmd', 'START_HERE.md', 'MANIFEST.txt', 'BUILD_INFO.txt', 'build-provenance.json', 'notices/THIRD_PARTY_LICENSES.txt'}
    inventory = []
    for line in (repo / 'src/common/PackageInventory.inc').read_text().splitlines():
        match = re.fullmatch(r'SF4E_PACKAGE_(REQUIRED|OPTIONAL|OBSOLETE)\("(.*)"\)', line)
        if not match:
            continue
        kind, raw = match.groups()
        relative = raw.replace('\\\\', '/')
        if relative in excluded:
            continue
        inventory.append(line)
        if kind == 'OBSOLETE' or relative in generated:
            continue
        # Fresh build output only; never fall back to a stale stage/candidate.
        candidates = [args.build_dir / relative, repo / relative, repo / '.github' / relative]
        if relative == 'notices/Dear-ImGui-MIT.txt':
            candidates.append(repo / 'src/ui/backends/LICENSE.txt')
        if relative.startswith('docs/'):
            candidates.extend((repo / 'docs').glob('*/' + Path(relative).name))
        source = next((p for p in candidates if p.is_file()), None)
        if source:
            copy(source, relative)
        elif kind == 'REQUIRED':
            raise FileNotFoundError(relative)

    extras = ['notices/NotoSansCJK-OFL.txt', 'Start with Native Display.cmd', 'PRIVATE_TEST_BUILD.txt']
    copy(repo / 'src/ui/fonts/NotoSansCJK-OFL.txt', extras[0])
    copy(repo / 'scripts/start-native-display.cmd', extras[1])
    (dest / extras[2]).write_text('Private local test build. Public updates are disabled to preserve these fixes.\nExtract newer test builds into separate folders. See START_HERE.md.\n', encoding='utf-8')
    # Ship the complete matching x86 CRT, including secondary imported DLLs.
    for source in sorted(args.crt.glob('*.dll')):
        copy(source, source.name)
        if not any(f'("{source.name}")' in line for line in inventory):
            extras.append(source.name)
    for relative in extras:
        escaped = relative.replace('/', '\\\\')
        inventory.append(f'SF4E_PACKAGE_REQUIRED("{escaped}")')
    (dest / 'PackageInventory.inc').write_text('// SDK-free private-test inventory; not an updater package.\n' + '\n'.join(inventory) + '\n', encoding='utf-8')
    art = repo / 'assets/selection'
    for source in art.rglob('*'):
        if not source.is_file() or source.suffix.lower() not in {'.png', '.jpg', '.json', '.md'}:
            continue
        if re.fullmatch(r'color-\d+\.(png|jpg)', source.name):
            if not source.with_name(source.stem + '-cutout.png').is_file():
                raise FileNotFoundError(f'Missing cutout for {source}')
            continue
        copy(source, 'assets/selection/' + source.relative_to(art).as_posix())

    # Keep public-release advice out of the test package's interactive preflight.
    preflight = (repo / 'scripts/tester-preflight.ps1').read_text()
    preflight = preflight.replace("    Write-Host 'To play, run Launcher.exe. To update, choose Check for updates in Help & about,'\n    Write-Host 'or double-click Updater.exe.'", "    Write-Host 'To test, run Launcher.exe. Do not update this private test build.'")
    (dest / 'preflight.ps1').write_text(preflight, encoding='utf-8-sig')
    (dest / 'preflight.cmd').write_text('@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0preflight.ps1" -PackageDir "%~dp0." -Interactive\n@pause\n', encoding='ascii')
    guide = repo / 'docs/guides/TEST_BUILD.md'
    copy(guide, 'START_HERE.md')
    receipt = json.loads(args.receipt.read_text(encoding='utf-8-sig'))
    receipt['label'] = args.label
    receipt['binaries'] = {p.name: sha(p) for p in dest.iterdir() if p.suffix.lower() in {'.exe', '.dll'}}
    (dest / 'build-provenance.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    (dest / 'BUILD_INFO.txt').write_text(f"SF4 Ember Netplay PRIVATE TEST BUILD\nBuild: {args.label}\nBase revision: {receipt['baseRevision']} plus local changes\nSource fingerprint: {receipt['sourceFingerprint']}\nDiscord disabled. Independent input delay; new profiles default to 0.\nRound-transition fix retained; prior candidate completed four native 0/0 matches.\nLauncher display controls: native, windowed, borderless, fullscreen, monitor, resolution, refresh.\nMain room includes chat history, message input and Send without opening another screen.\nPrior borderless prototype accepted by user; expanded modes still need native playtests.\nPublic updates disabled. Not a public release or updater package.\nSee START_HERE.md and build-provenance.json.\n", encoding='utf-8')
    run(sys.executable, '-X', 'utf8', repo / 'scripts/collect-notices.py', '--build-dir', args.build_dir, '--output', dest / 'notices/THIRD_PARTY_LICENSES.txt')
    run(sys.executable, '-X', 'utf8', repo / 'scripts/package-docs.py', '--repo', repo, '--package', dest, '--revision', receipt['baseRevision'], '--quick-start', guide)
    manifest = '\n'.join(f'{sha(p)}  {str(p.relative_to(dest))}' for p in sorted(dest.rglob('*')) if p.is_file()) + '\n'
    (dest / 'MANIFEST.txt').write_text(manifest, encoding='utf-8')
    run('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', dest / 'preflight.ps1', '-PackageDir', dest, '-Strict')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for source in sorted(dest.rglob('*')):
            if source.is_file():
                output.write(source, dest.name + '/' + source.relative_to(dest).as_posix())
    verify = staging / (args.label + '-extracted')
    verify.mkdir(exist_ok=False)
    with zipfile.ZipFile(archive) as packaged:
        if packaged.testzip() is not None:
            raise ValueError('ZIP integrity check failed')
        packaged.extractall(verify)
    run('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', verify / dest.name / 'preflight.ps1', '-PackageDir', verify / dest.name, '-Strict')
    archive.with_name(archive.name + '.sha256').write_text(f'{sha(archive)}  {archive.name}\n', encoding='ascii')
    # The ZIP and playable dist folder are retained; the verified extraction
    # is disposable scratch and otherwise duplicates every private build.
    remove_verified_extraction(verify, staging)
    game, share, saved = publish_private_build(repo / 'dist', archive, repo / 'build/private-packages')
    print(f'Play: {repo / "dist/PLAY.cmd"}')
    print(f'Share: {share} ({share.stat().st_size / 1048576:.1f} MiB)')
    if saved:
        print(f'Preserved previous player files: {saved}')


if __name__ == '__main__':
    main()
