# Ultra input symbols

The Ultra selector displays vector motion arrows and arcade button symbols beneath each move name. Ultra Double shows both complete commands. Ultra photos are no longer displayed. Costume and stage photos retain their existing behavior.

Notation faces right. An orange dot marks the start of a curved motion; HOLD marks a charge direction. Three P or K buttons mean simultaneous punches or kicks. LP, HP and LK indicate individual strengths in Akuma's sequence. Circular motions are shown twice for 720-degree commands. Air, stance, counter and proximity conditions accompany their commands.

## Sources

- [EventHubs Abel move list](https://www.eventhubs.com/moves/sf4/abel/), inspected in the browser: the requested visual reference, with two quarter-circle arrows and three punch/kick symbols. The launcher draws its own vector symbols; no EventHubs symbol image files are copied.
- [Capcom / Taito USFIV arcade command sheets](https://usf4.nesica.net/command-list.html): the 43 pre-Decapre characters. Cached originals and exact page/image mappings are in `build/fighter-selection/commands/nesica-sources.json`.
- [EventHubs Decapre move list](https://www.eventhubs.com/moves/sf4/decapre/): Ultra I uses horizontal charge and three punches; Ultra II has horizontal and diagonal charge variants, both with three kicks.
- [Capcom's SSFIV M. Bison development notes](https://news.capcomusa.com/lets/browse/super-street-fighter-iv-dev-blog-changes-to-guile-dhalsim-balrog-vega-and-m-bison): original SSFIV Psycho Punisher uses a motion command. AE and later use charge.
- [Capcom Omega Decapre introduction](https://news.capcomusa.com/lets/browse/an-introduction-to-the-usfiv-omega-edition-characters-part-vii): Omega changes her from charge to motion inputs.
- [Contemporary Omega Decapre command table](https://w.atwiki.jp/ssf4/pages/4415.html): Psycho Stream uses two quarter circles forward plus PPP; DCM ground uses two quarter circles forward plus KKK; anti-air uses two quarter circles back plus KKK. This is a community reference; the linked official 1.05 PDF no longer exists at its original URL.

`UltraCommands` rejects unsupported fighter/edition/Ultra combinations. Ultra Double is composed of I and II, not a third command. Commands are informational and do not alter native input handling.

Validation covers the selectable catalog and specifically checks SSFIV/AE Bison, standard/Omega Decapre, Gen's air stance, and invalid IDs. Real DX9 rendering covers motion, charge, stance and multiple-command layouts at five viewport/DPI configurations. This does not prove execution of each move in native gameplay.
