# Attribution

## Important: unofficial port

**SF4 Ember Netplay is an experimental unofficial port.** It is **not** the official [sf4e](https://codeberg.org/adanducci/sf4e) project, and it is **not** affiliated with or endorsed by **Anthony Danducci**, Capcom, or Valve. It is **not production-ready software** and should not be presented as stable or "working" netplay.

For the official upstream project and updates, use Anthony Danducci's sf4e on Codeberg, not this repository.

## Upstream project (sf4e)

This port is based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci** and contributors (MIT).

- Upstream repository: [codeberg.org/adanducci/sf4e](https://codeberg.org/adanducci/sf4e)
- Mirror: [gitlab.com/adanducci/sf4e](https://gitlab.com/adanducci/sf4e)

Anthony Danducci's sf4e is a process-inspection and modification tool for the Steam release of *Ultra Street Fighter IV*, including rollback netplay via GGPO and GameNetworkingSockets.

**Anthony Danducci does not maintain this port.**

## This port (SF4 Ember Netplay)

Community additions in this unofficial port:

- Fixed in-game ImGui shell and Win32/DX9 launch recovery
- Iroh private invitations and authenticated loopback gameplay bridging
- Release scripts and tester documentation

**Scope:** USF4 on Steam, Windows 10+, rollback netplay for a **small friends group**. **Not** a commercial matchmaking service. **Not production-ready software.**

**Limitations:** experimental netplay. Actual SF4 rematch, disconnect, spectator, different-network and clean-machine results remain acceptance gates. See [scope](docs/guides/SCOPE_AND_LIMITATIONS.md).

Maintained at: [github.com/Confetti3/SF4-Ember-Netplay](https://github.com/Confetti3/SF4-Ember-Netplay)

## License

This project remains under the [MIT License](LICENSE), consistent with upstream sf4e. The MIT copyright notice from upstream must be preserved in all copies and substantial portions of the Software.

## Third-party and game notices

The ImGui interface embeds unmodified **Inter Regular** and **Inter SemiBold**,
by Rasmus Andersson and the [Inter Project Authors](https://github.com/rsms/inter),
under the [SIL Open Font License 1.1](src/ui/fonts/OFL.txt). Font provenance and
hashes are recorded in [src/ui/fonts/SOURCE.txt](src/ui/fonts/SOURCE.txt). The full
license is also available in the interface's About page and packaged notices.

See [README.md](README.md#external-licenses-and-copyright-information) for dependency and trademark notices (Capcom, Valve, Microsoft, etc.). Packages include `notices/THIRD_PARTY_LICENSES.txt` and the Discord Social SDK notice in `notices/Discord-SDK.txt`.

You must own *Ultra Street Fighter IV* on Steam to play.

## Fighter selection reference photographs

The optional selection photos contain Capcom game imagery. The alternate-outfit
gallery is credited to [Street Fighter Galleries / slateman](https://www.sfgalleries.net/art/sf4/usf4/alt/),
and the Horror images come from [Capcom's 2015 costume announcement](https://news.capcomusa.com/lets/browse/beware-ultra-street-fighter-iv-horror-costumes-are-coming).
Their individual source URLs and hashes are recorded in
[assets/selection/sources.json](assets/selection/sources.json). Transparent cutouts
are locally masked and cropped derivatives of those photographs. Capcom imagery
is separate from the MIT license covering the launcher code.

Stage screenshots are credited to
[SuperSoluce / Pierre Durden, June 16, 2014](https://www.supersoluce.com/soluce/ultra-street-fighter-iv/stages).
They are resized to 640x360 with the HUD and watermark preserved. Their source
URLs and original/output hashes are in
[assets/selection/stage-sources.json](assets/selection/stage-sources.json).
These screenshots contain Capcom game imagery and are not covered by the
launcher's MIT license.

The Random stage card is not packaged: like the native fighter portraits, it
reads the game's character-select random tile from the installed game at
runtime and shows the preview-unavailable placeholder when that tile cannot be read.

Ultra Combo photographs are from [iPlayWinner's 2010 SSFIV character guides](https://iplaywinner.squarespace.com/ryu-ssf4/),
[Capcom / ComboFiend's November 2013 Ultra showcase](https://news.capcomusa.com/lets/browse/ultras-and-supers-for-the-new-usfiv-characters),
and [GamesRadar's 2014 Decapre preview](https://www.gamesradar.com/ultra-street-fighter-4s-decapre-plays-nothing-cammy/).
The photographs are cropped/resized to 256x144, with source headings, native
move mappings, URLs, and hashes in `assets/selection/ultra-sources.json`.

Numbered color photographs are from EventHubs' SFIV/SSFIV costume guides,
[indexed June 20, 2010](https://plaza.rakuten.co.jp/dhomochevsky/diary/201006200000/).
`scripts/selection-color-sheets.json` pins original sheet hashes and crop
rectangles; `assets/selection/color-sources.json` records each numbered photo.
Cropping and local alpha masking preserve the photographed costume and palette.
These Ultra and color photographs contain Capcom game imagery and are separate
from the launcher's MIT license.

The retained Ultra archive also includes Capcom materials published by 4Gamer:
[Yun and Yang (2010)](https://www.4gamer.net/games/111/G011109/20100921073/),
[Evil Ryu (2011)](https://www.4gamer.net/games/111/G011109/20110325017/), and
[Oni (2011)](https://www.4gamer.net/games/111/G011109/20110408015/).
Ultra selection now uses locally drawn vector inputs, with visual notation
referenced from [EventHubs' Abel guide](https://www.eventhubs.com/moves/sf4/abel/).
See `docs/design/ULTRA_INPUTS.md` for command sources and edition exceptions.

## Controller prompts and menu background

Controller and keyboard prompt sprites are unmodified assets from
[Kenney Input Prompts 1.5A](https://kenney.nl/assets/input-prompts), licensed CC0.
The distributed license is `assets/input/License.txt`; source archive identity
and the selected asset directories are recorded in `assets/input/README.md`.

The Ember menu background was generated specifically for this project. Its
generation prompt and content hash are recorded in `assets/brand/README.md`.
