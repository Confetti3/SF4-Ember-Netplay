"""Collect notices from the resolved Windows dependency graph used for a package."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
cache = (args.build_dir / 'CMakeCache.txt').read_text()
installed = Path(re.search(r'^VCPKG_INSTALLED_DIR:[^=]+=(.+)$', cache, re.M)[1])
triplet = re.search(r'^VCPKG_TARGET_TRIPLET:[^=]+=(.+)$', cache, re.M)[1]
sections = ['SF4 Ember Netplay — dependency notices\nGenerated from the resolved Windows build.\n']
native = ['cli11', 'detours', 'fmt', 'ggpo', 'imgui', 'nlohmann-json', 'spdlog', 'valvefilevdf', 'wil', 'zlib']
for name in native:
    license_path = installed / triplet / 'share' / name / 'copyright'
    sections.append(f'\n=== {name} (vcpkg) ===\n' + license_path.read_text(encoding='utf-8'))
cargo = Path(os.environ.get('CARGO_HOME', str(Path.home() / '.cargo'))) / 'bin' / 'cargo.exe'
metadata = json.loads(subprocess.check_output([str(cargo), 'metadata', '--locked', '--format-version', '1',
    '--filter-platform', 'x86_64-pc-windows-msvc'], cwd=repo/'rust/sf4-net'))
nodes = {node['id']: node for node in metadata['resolve']['nodes']}
pending = [metadata['resolve']['root']]
reachable = set()
while pending:
    package_id = pending.pop()
    if package_id in reachable:
        continue
    reachable.add(package_id)
    pending.extend(nodes[package_id]['dependencies'])
count = 0
for package in sorted(metadata['packages'], key=lambda item: (item['name'], item['version'])):
    if package['id'] not in reachable or package['name'] == 'sf4-net':
        continue
    directory = Path(package['manifest_path']).parent
    heading = f"\n=== {package['name']} {package['version']} ===\nLicense: {package.get('license') or 'See license file'}\nSource: {package.get('repository') or package.get('homepage') or package['source']}\n"
    licenses = sorted(path for path in directory.iterdir() if path.is_file() and
        (path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE'))))
    if package.get('license_file'):
        explicit = directory/package['license_file']
        if explicit not in licenses:
            licenses.append(explicit)
    sections.append(heading)
    for license_path in licenses:
        sections.append(f'\n{license_path.name}\n' + license_path.read_text(encoding='utf-8', errors='replace'))
    if not licenses:
        # Some crates declare their license only through Cargo metadata. Preserve
        # that declaration and the source attribution rather than inventing text.
        sections.append('The crate archive declares the license above in Cargo.toml and contains no separate license text.\n')
    count += 1
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('\n'.join(sections), encoding='utf-8')
print(f'Collected notices for {len(native)} native packages and {count} Rust dependencies.')
