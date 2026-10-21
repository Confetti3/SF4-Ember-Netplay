// Each award's emblem: a plain round badge in Ember's palette
// (src/ui/Theme.cxx: Ink, Panel, Border, Ivory, Muted, Ember), with a mark
// for its kind. Made here, so there are no image files to keep.
import { TITLES, type Kind } from "./awards.ts";

const INK = "#141312";
const PANEL = "#211E1B";
const BORDER = "#443A31";
const IVORY = "#F3EBDD";
const MUTED = "#B5A99B";
const EMBER = "#FF8738";

const STAR = "M32 14l5.3 10.8 11.9 1.7-8.6 8.4 2 11.9L32 41.2l-10.6 5.6 2-11.9-8.6-8.4 11.9-1.7z";

/** Each kind's mark: a star, or one or two letters, and its ring's colour. */
const MARKS: Record<Kind, { ring: string; star?: boolean; text?: string }> = {
  champion: { ring: EMBER, star: true },
  runner_up: { ring: IVORY, star: true },
  top_3: { ring: MUTED, text: "3" },
  top_8: { ring: BORDER, text: "8" },
  giant_killer: { ring: EMBER, text: "GK" },
  perfectionist: { ring: IVORY, text: "P" },
  marathon: { ring: MUTED, text: "M" },
  iron: { ring: MUTED, text: "Fe" },
  one_trick: { ring: BORDER, text: "1" },
  mixer: { ring: BORDER, text: "Mx" },
  season_champion: { ring: EMBER, star: true, text: "S" },
  season_top_3: { ring: MUTED, text: "S3" },
  best_player: { ring: EMBER, text: "BP" },
  best_combo: { ring: EMBER, text: "BC" },
  best_moment: { ring: EMBER, text: "BM" },
};

export function isKind(kind: string): kind is Kind {
  return kind in MARKS;
}

export function emblem(kind: Kind): string {
  const mark = MARKS[kind];
  // A season's champion has its star small, with its letter under it.
  const star = mark.star ? `<path d="${STAR}" fill="${mark.ring}"${mark.text ? ' transform="translate(9.6 4) scale(.7)"' : ""}/>` : "";
  const size = mark.text && mark.text.length > 1 ? 18 : 24;
  const y = mark.star ? 52 : 40;
  const text = mark.text
    ? `<text x="32" y="${y}" text-anchor="middle" font-family="Inter, sans-serif" font-weight="600" font-size="${mark.star ? 12 : size}" fill="${mark.star ? IVORY : mark.ring}">${mark.text}</text>`
    : "";
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="${TITLES[kind]}">
<circle cx="32" cy="32" r="30" fill="${PANEL}" stroke="${mark.ring}" stroke-width="4"/>
<circle cx="32" cy="32" r="24" fill="${INK}" stroke="${BORDER}" stroke-width="1"/>
${star}${text}
</svg>
`;
}
