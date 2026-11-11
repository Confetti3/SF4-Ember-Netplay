// Writes the line icons (src/ui/LineIcons.inc) from a Tabler Icons release.
//
//   node scripts/build-line-icons.mjs --tabler <@tabler/icons package folder>
//
// The folder is the unpacked npm package (npm pack @tabler/icons@3.19.0;
// tabler-icons-3.19.0.tgz, SHA-256
// 6188551a8b8bd1fb31a75d219e1db499b60ee7e7f9195467fe2268c72fd9b631).
//
// Each icon below is one or more Tabler outline icons, their paths joined in
// order on Tabler's 24 grid. The background square every Tabler icon starts
// with is left out. Each path is checked against what LineIcons.cxx strokes
// (normalized), so a new release cannot slip in something the stroker would
// misread. Also copies the
// release's LICENSE to src/ui/TablerIcons-MIT.txt, which collect-notices.py
// puts in the package's notices.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const at = args.indexOf('--tabler');
const tabler = at >= 0 ? args[at + 1] : null;
if (!tabler) { console.error('--tabler <@tabler/icons package folder> is required'); process.exit(1); }

// LineIcon's names in its order, and the Tabler icons each is drawn from.
const icons = [
  // Settings panels.
  ['User', ['user']],
  ['World', ['world']],
  ['Id', ['id']],
  ['Adjustments', ['adjustments-horizontal']],
  ['Movie', ['movie']],
  ['Palette', ['palette']],
  ['LayoutNavbar', ['layout-navbar']],
  ['LayoutDashboard', ['layout-dashboard']],
  ['Volume', ['volume']],
  ['Speakerphone', ['speakerphone']],
  ['Typography', ['typography']],
  ['Help', ['help-circle']],
  ['Discord', ['brand-discord']],
  ['ChartBar', ['chart-bar']],
  ['Info', ['info-circle']],
  ['Bug', ['bug']],
  // The hint bar's notices.
  ['Bell', ['bell']],
  // The award icons, in v2::Icon's order.
  ['Flame', ['flame']],
  ['History', ['history']],
  ['Laurel', ['laurel-wreath']],
  ['CalendarCheck', ['calendar-check']],
  ['Octagon8', ['octagon', 'number-8-small']],
  ['Wave', ['wave-sine']],
  ['Medal', ['medal-2']],
  ['Flag', ['flag']],
  ['Microphone', ['microphone']],
  ['Tournament', ['tournament']],
  ['ShieldStar', ['shield-star']],
  ['Crown', ['crown']],
  ['Brush', ['brush']],
  // The Friends panel.
  ['Users', ['users']],
];

// `d` checked against what LineIcons.cxx strokes and written back as its
// tokens one space apart: the commands M L H V C S A Z (either case), each
// argument group complete, implicit repeats included, and every arc a circle
// (equal radii above 0) between two different points.
function normalized(source, d) {
  const tokens = d.match(/[MmLlHhVvCcSsAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g) ?? [];
  if (d.replace(/[MmLlHhVvCcSsAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|[ ,]/g, '') !== '')
    throw new Error(`${source}: the stroker does not read all of ${d}`);
  const counts = { M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, A: 7, Z: 0 };
  let command = null, x = 0, y = 0, sx = 0, sy = 0;
  for (let i = 0; i < tokens.length;) {
    if (/^[MmLlHhVvCcSsAaZz]$/.test(tokens[i])) command = tokens[i++];
    else if (!command || command.toUpperCase() === 'Z') throw new Error(`${source}: a number with no command in ${d}`);
    const upper = command.toUpperCase(), rel = command !== upper, n = counts[upper];
    const args = tokens.slice(i, i + n).map(Number);
    if (args.length < n || args.some(Number.isNaN)) throw new Error(`${source}: ${command} wants ${n} numbers in ${d}`);
    i += n;
    const ox = rel ? x : 0, oy = rel ? y : 0;
    switch (upper) {
      case 'M': x = ox + args[0]; y = oy + args[1]; sx = x; sy = y; command = rel ? 'l' : 'L'; break;
      case 'L': x = ox + args[0]; y = oy + args[1]; break;
      case 'H': x = ox + args[0]; break;
      case 'V': y = oy + args[0]; break;
      case 'C': x = ox + args[4]; y = oy + args[5]; break;
      case 'S': x = ox + args[2]; y = oy + args[3]; break;
      case 'A': {
        const [rx, ry] = args, tx = ox + args[5], ty = oy + args[6];
        if (rx !== ry || !(rx > 0)) throw new Error(`${source}: the stroker draws circular arcs only, not ${rx} by ${ry}`);
        if (tx === x && ty === y) throw new Error(`${source}: an arc that ends where it starts in ${d}`);
        x = tx; y = ty;
        break;
      }
      case 'Z': x = sx; y = sy; break;
    }
  }
  return tokens.join(' ');
}

const pkg = JSON.parse(fs.readFileSync(path.join(tabler, 'package.json'), 'utf8'));
const nodes = JSON.parse(fs.readFileSync(path.join(tabler, 'tabler-nodes-outline.json'), 'utf8'));
const lines = [];
for (const [name, sources] of icons) {
  const paths = [];
  for (const source of sources) {
    const icon = nodes[source];
    if (!icon) throw new Error(`Tabler ${pkg.version} has no outline icon ${source}`);
    for (const [tag, attributes] of icon) {
      if (tag !== 'path') throw new Error(`${source}: <${tag}> is not a path`);
      const d = attributes.d.trim();
      if (d === 'M0 0h24v24H0z') continue;
      paths.push(normalized(source, d));
    }
  }
  lines.push(`SF4E_LINE_ICON(${name}, "${sources.join(' + ')}", "${paths.join(' ')}")`);
}

const header = `// Generated by scripts/build-line-icons.mjs from Tabler Icons ${pkg.version}
// (MIT, src/ui/TablerIcons-MIT.txt): each line icon's name, the Tabler icons
// it is drawn from, and their paths on Tabler's 24 grid. Do not edit by hand.
`;
fs.writeFileSync(path.join(root, 'src', 'ui', 'LineIcons.inc'), header + lines.join('\n') + '\n');
fs.copyFileSync(path.join(tabler, 'LICENSE'), path.join(root, 'src', 'ui', 'TablerIcons-MIT.txt'));
console.log(`Wrote ${lines.length} line icons from Tabler Icons ${pkg.version}.`);
