# Ultra input symbols

The Ultra cards (`AppearanceGallery`, the Ultras page over a fighter's page)
are the old Ultra page's look at the size of the screen: one card per Ultra the
edition allows, side by side and as wide as three fit (`UltraCard` in
`AppearanceGrid.hxx`), the row centred on the page. Each card shows the Ultra's
photo with its mark in the top-right corner and SAVED top-left on the saved
one; Ultra Double shows a large W instead of a photo. Under the photo come the
Ultra's name, then for each command its vector motion arrows and arcade button
symbols, and under the symbols the same input in words that face neither way:
a line per command from `UltraInputs`, which joins `UltraNotation` with the
command's `UltraCondition` in the player's language, such as "Charge B, F, B, F
+ KKK (Ground)". Ultra Double shows both Ultras' commands, each under U1 or U2
and the move's name. An Ultra with no packaged photo shows the fighter in what
they wear. The room bar's Ultra button and Table options' Ultra row open the
same cards; Left and Right on the fighter page's Ultra row still step it in
place. The commands are one table in native fighter order plus the editions
that differ (SSFIV Bison's Ultra II, Omega Decapre).

The mark is the game's own: the green I, purple II and flaming W of USFIV's
character select (`uc1.dds` to `uc3.dds`), read at runtime from the game's
`chara_select` archives by `SelectionArt::UltraIcon`, never packaged. The W is
only in Ultra's own archive (`dlc/04_ae2`), so it is read only there; the I
and II fall back to the older archives. The 32-pixel icons are drawn on whole
pixels at no more than twice their size. Where the game has none, the card
draws the old badge instead: I, II or W in Ember on a dark plate
(`DrawUltraMark`). An Arcade or King of the hill cabinet shows the seated
player's mark in the top-right corner of its screen, over the fighter as on the
Ultra cards, once their Ultra is known; a screen too small for it (the narrow
layout's) shows none, and an empty cabinet none. The marquee keeps only the
cabinet number (and a streak).

Notation faces right. An orange dot marks the start of a curved motion; the word for a charge (the translation of Charge) marks a charge direction. Three P or K buttons mean simultaneous punches or kicks. LP, HP and LK indicate individual strengths in Akuma's sequence: Ultra I (Wrath of the Raging Demon) is LP, LP, forward, LK, HP. Circular motions are shown twice for 720-degree commands. Air, stance, counter and proximity conditions accompany their commands.

## Sources

- [EventHubs Abel move list](https://www.eventhubs.com/moves/sf4/abel/), inspected in the browser: the requested visual reference, with two quarter-circle arrows and three punch/kick symbols. The launcher draws its own vector symbols; no EventHubs symbol image files are copied.
- [Capcom / Taito USFIV arcade command sheets](https://usf4.nesica.net/command-list.html): the 43 pre-Decapre characters. Cached originals and exact page/image mappings are in `build/fighter-selection/commands/nesica-sources.json`.
- [EventHubs Decapre move list](https://www.eventhubs.com/moves/sf4/decapre/): Ultra I uses horizontal charge and three punches; Ultra II has horizontal and diagonal charge variants, both with three kicks.
- [Capcom's SSFIV M. Bison development notes](https://news.capcomusa.com/lets/browse/super-street-fighter-iv-dev-blog-changes-to-guile-dhalsim-balrog-vega-and-m-bison): original SSFIV Psycho Punisher uses a motion command. AE and later use charge.
- [Capcom Omega Decapre introduction](https://news.capcomusa.com/lets/browse/an-introduction-to-the-usfiv-omega-edition-characters-part-vii): Omega changes her from charge to motion inputs.
- [Contemporary Omega Decapre command table](https://w.atwiki.jp/ssf4/pages/4415.html): Psycho Stream uses two quarter circles forward plus PPP; DCM ground uses two quarter circles forward plus KKK; anti-air uses two quarter circles back plus KKK. This is a community reference; the linked official 1.05 PDF no longer exists at its original URL.

`UltraCommands` rejects unsupported fighter/edition/Ultra combinations. Ultra Double is composed of I and II, not a third command. Commands are informational and do not alter native input handling.

Validation covers the selectable catalog and specifically checks SSFIV/AE Bison, standard/Omega Decapre, Gen's air stance, and invalid IDs. Real DX9 rendering covers motion, charge, stance and multiple-command layouts at five viewport/DPI configurations. This does not prove execution of each move in native gameplay.
