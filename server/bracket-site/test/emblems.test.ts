import assert from "node:assert/strict";
import { test } from "node:test";

import { TITLES, type Kind } from "../src/awards.ts";
import { emblem, isKind } from "../src/emblems.ts";

test("every award kind has an emblem in Ember's palette, named for screen readers", () => {
  for (const kind of Object.keys(TITLES) as Kind[]) {
    const svg = emblem(kind);
    assert.match(svg, /^<svg xmlns="http:\/\/www.w3.org\/2000\/svg" viewBox="0 0 64 64"/, kind);
    assert.ok(svg.includes(`aria-label="${TITLES[kind]}"`), kind);
    assert.ok(svg.includes('fill="#211E1B"') && svg.includes('fill="#141312"'), kind);
    // Nothing but Ember's colours.
    for (const colour of svg.match(/#[0-9A-F]{6}/g)!) assert.ok(["#141312", "#211E1B", "#443A31", "#F3EBDD", "#B5A99B", "#FF8738"].includes(colour), `${kind}: ${colour}`);
  }
  assert.match(emblem("champion"), /<path d="M32 14/);
  assert.match(emblem("giant_killer"), />GK<\/text>/);
  assert.equal(isKind("admin"), false);
});
