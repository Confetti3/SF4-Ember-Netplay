# Ultra input symbols

Since 1.6.0 the Ultra cards (`AppearanceGallery`, the Ultras page over a
fighter's page) show each Ultra's picture, its name and its input in words: a
line per command from `UltraInputLines`, which joins `UltraNotation` with the
command's `UltraCondition` in the player's language, such as "Charge B, F, B, F
+ KKK (Ground)". Ultra Double shows Ultra I's picture beside Ultra II's and both
inputs, each after U1 or U2. An Ultra with no packaged picture shows the
fighter in what they wear. The room bar's Ultra button and Table options' Ultra
row open the same cards. The vector arrows and button symbols described below
were drawn by the Classic look's Ultra page, retired on 2026-10-30; the cards
use the words alone. The commands are one table in native fighter order plus
the editions that differ (SSFIV Bison's Ultra II, Omega Decapre).

The Ultra selector displays vector motion arrows and arcade button symbols beneath each move name. Ultra Double shows both complete commands. Under the arrows, one line gives the same input in facing-neutral notation (QCF x2, Charge B, F, B, F, U, U), made by `UltraNotation`, so it reads the same whichever side the player starts on. The Ultra cards show each Ultra's photo with a badge in the top-right corner (I, II, or W for Ultra Double, which shows a large W instead of a photo); SAVED stays top-left.

Notation faces right. An orange dot marks the start of a curved motion; HOLD marks a charge direction. Three P or K buttons mean simultaneous punches or kicks. LP, HP and LK indicate individual strengths in Akuma's sequence: Ultra I (Wrath of the Raging Demon) is LP, LP, forward, LK, HP. Circular motions are shown twice for 720-degree commands. Air, stance, counter and proximity conditions accompany their commands.

## Sources

- [EventHubs Abel move list](https://www.eventhubs.com/moves/sf4/abel/), inspected in the browser: the requested visual reference, with two quarter-circle arrows and three punch/kick symbols. The launcher draws its own vector symbols; no EventHubs symbol image files are copied.
- [Capcom / Taito USFIV arcade command sheets](https://usf4.nesica.net/command-list.html): the 43 pre-Decapre characters. Cached originals and exact page/image mappings are in `build/fighter-selection/commands/nesica-sources.json`.
- [EventHubs Decapre move list](https://www.eventhubs.com/moves/sf4/decapre/): Ultra I uses horizontal charge and three punches; Ultra II has horizontal and diagonal charge variants, both with three kicks.
- [Capcom's SSFIV M. Bison development notes](https://news.capcomusa.com/lets/browse/super-street-fighter-iv-dev-blog-changes-to-guile-dhalsim-balrog-vega-and-m-bison): original SSFIV Psycho Punisher uses a motion command. AE and later use charge.
- [Capcom Omega Decapre introduction](https://news.capcomusa.com/lets/browse/an-introduction-to-the-usfiv-omega-edition-characters-part-vii): Omega changes her from charge to motion inputs.
- [Contemporary Omega Decapre command table](https://w.atwiki.jp/ssf4/pages/4415.html): Psycho Stream uses two quarter circles forward plus PPP; DCM ground uses two quarter circles forward plus KKK; anti-air uses two quarter circles back plus KKK. This is a community reference; the linked official 1.05 PDF no longer exists at its original URL.

`UltraCommands` rejects unsupported fighter/edition/Ultra combinations. Ultra Double is composed of I and II, not a third command. Commands are informational and do not alter native input handling.

Validation covers the selectable catalog and specifically checks SSFIV/AE Bison, standard/Omega Decapre, Gen's air stance, and invalid IDs. Real DX9 rendering covers motion, charge, stance and multiple-command layouts at five viewport/DPI configurations. This does not prove execution of each move in native gameplay.
