# The V2 kit

Every screen is built from the same parts. They live in `src/ui/V2.hxx`
(drawing only, namespace `v2`) and, for the parts that know about rows and
navigation, `src/ui/GameMenu.hxx`. The design boards are the V2 canvas
(`lab/v2-mockups/v2canvas/project/*.dc.html`); `Kit.dc.html` is the component
sheet these parts follow.

## Parts

### Look and layout (V2.hxx)

| Part | Use it for | Call |
| --- | --- | --- |
| Palette | Every colour: ink, frame, panel, button, header, hover, border, accent, accentLight, text, muted, live, danger, ready | `v2::Colors().accent` |
| Alpha | A palette colour at an alpha | `v2::color::Alpha(c.border, .6f)` |
| Margins | Side margin and padding for a width | `v2::Pad`, `v2::SideMargin` |
| Narrow | The one narrow test (under 900 logical px) | `v2::Narrow(ImGui::GetWindowWidth(), s)` |
| Box | Lay a screen out by cutting boxes off a box | `v2::CutTop/CutBottom/CutLeft/CutRight`, `v2::Inset` |

### Grounds and marks (V2.hxx)

| Part | Use it for | Call |
| --- | --- | --- |
| BoardGround | The window or child behind a whole board | `v2::BoardGround(seeThrough)` |
| PanelGround | A panel inside a board (.94, .78 over a watched game) | `v2::PanelGround(seeThrough)` |
| Panel | A titled panel: ground, edge, 40 px head (taller for a wrapped title), rule; returns the body | `v2::Panel(d, box, title, trailing, open)` |
| CardGround | A free-standing card or row (cards, tiles, fighter rows) | `v2::CardGround(d, box, focused, hovered, act)` |
| RowGround | A row inside a panel (flat: nothing at rest) | `v2::RowGround(d, box, focused, hovered)` |
| Focus | The focused thing's marks, drawn over it | `v2::Focus(d, min, max, bar)` |

### Words (V2.hxx)

| Part | Use it for | Call |
| --- | --- | --- |
| Text | One line fitted to a width, optionally centred; the text probe sees it | `v2::Text(d, at, width, text, size, color, font, centred)` |
| Fit | The fitted string, when you draw it yourself | `v2::Fit(text, width, size, font)` |
| Heading | A heading that wraps rather than cut (a Settings rail head, a panel's title, a row's label wider than its row): one line when it fits, else as many whole words as fit and the rest a second line `v2::HeadingLine` (1.2) under, its box growing by that line; in the heading font unless given one | `v2::Heading(d, at, width, text, size, color, font)`, height first `v2::HeadingHeight(text, width, size, font)` |
| HeadWord | A head's state word (a panel's Open, a rail head's Closed): it stands while the heading fits beside it on one line and gives way before the heading wraps | `v2::HeadWord(heading, size, word, room, gap)`, 0 when it gave way |
| SectionHeading | A group's heading (Kit: eyebrow), 14 px, muted | `v2::SectionHeading(d, at, text, tone)` |
| Paragraph | Wrapped text at the boards' 1.45 line height | `v2::Paragraph(text, size, color)` at the cursor, or `v2::Paragraph(d, at, width, ...)` which returns its height |
| Wrapped | `TextWrapped` that wraps like a Paragraph | `v2::Wrapped(text)` |
| Words | The focused row's label and detail under a board | `v2::Words(d, box, label, detail, wrap)`, height from `v2::WordsHeight(width, detail)` |
| Fact | A key and value line in a detail pane | `v2::Fact(key, value)` |
| Chip | A state word on its tone | `v2::Chip(d, at, text, tone)` |

Words that do not fit give way in one order: a state or trailing word first
(HeadWord; the header's eyebrow drops whole parts), then a heading wraps onto a
second line and its box grows (Heading), as a row's value goes under its label
(FitMenuRow) and then, where the label alone is wider than its whole row, the
label wraps the same way and the row grows by one more line (LayOutMenuRow).
Only a word wider than its room alone is cut, and the probe reports it; a
player's own words keep to one line and elide; a translation that still does
not fit is shortened in its catalog.

### Controls (V2.hxx)

| Part | Use it for | Call |
| --- | --- | --- |
| Button | A board button, 48 px: Primary (one per screen), Secondary, Danger (Leave) | `v2::Button(d, box, label, v2::ButtonKind::Primary, focused, hovered, enabled)` |
| Chevron | A row whose Select opens something | `v2::Chevron(d, at, size, color)` |
| Glow | A seat lit up: dim when free, brighter when taken, the pair's colour when playing | `v2::Glow(d, min, max, color, strength, reach, rounding)` |
| Header | A screen's brand, eyebrow, title, chips and Back | `v2::Header(title, eyebrow, chips, back)` |
| Tabs | Horizontal tabs under the header; the end marks step them, wrapping | `v2::Tabs(labels, current)` |
| HintBar | The bottom bar: glyph hints and the status | `v2::HintBar(bar, hints, status, tone, bell)` |
| SpotMap, Grip | Picking a spot, dragging a layout edge | `v2::SpotMap`, `v2::Grip` |
| Tag | A player: XS, S, M, L | `v2::Tag(d, at, width, view, v2::TagSize::S)` |
| Tier, Icon, Awards | Titles, icons and their tiers | `v2::TierColor`, `v2::Icon`, `v2::FindAward` |
| BrandMark | The brand's mark | `v2::BrandMark(d, at, height)` |

### Rows (GameMenu.hxx)

| Part | Use it for | Call |
| --- | --- | --- |
| Board value row | A value row on a board, whole: laid out once (`LayOutBoardRow`), pressed, its ground, words, focus marks and value panel anchor | `menu_.DrawBoardRow(rows, e, at, width, action, {column, enabled, focusChanged, choose, card})`, its height first `BoardRowHeight(e, width)` |
| PressEntry | A row's box as a button: pointer focuses, press is Select (or steps the value where it is drawn), scrolls into view | `PressEntry(nav, rows, e, box, action, focusChanged)`; a value row `PressEntry(..., focusChanged, layout)` |
| Row layout | A value row's whole box from its one-line box, and where its label and value lie: beside, or the value under, and the label on two lines when it is wider than its whole row; height, drawing and the pointer read the same one, so a click steps a value only on its own line | `LayOutBoardRow(e, at, width)` (`LayOutMenuRow(line, left, right, valueWidth, under, underLine, gap, labelLines)`, `MenuValueStep` in MenuNavigation.hxx) |
| DrawMenuRow | A value row's words: label left, value right (arrows, switch or chevron), or under it when both do not fit, the label on the lines its layout gave it (`v2::Heading` for two) | `DrawMenuRow(d, e, layout, enabled)` |
| FitMenuRow | How a row's label and value share its line: the value wants `MenuValueWidth(e)`, gives way to a floor, then goes under the label (the row grows by `v2::UnderLine`) | `FitMenuRow(e, room, gap, grows)` |
| ShareLine | The same rule for any label and value pair (a fact, a panel's title and count) | `v2::ShareLine(label, value, least, room, gap, grows)` |
| BarLines, BarPlaces | A bar of buttons on as few lines as their labels allow, each place as wide as it needs | `v2::BarPlaces(bar, need, gap, v2::BarLines(width, need, gap), height)` |
| DrawMenuValue | Only the value part of a row | `DrawMenuValue(d, e, box, enabled, textColor)` |
| Cards, detail, value panel | A list screen with no board of its own | `GameMenu::Draw` draws the rows itself |
| Value panel on a board | The choices of a board's focused row, as many across as fit the column | `DrawBoardRow` sets it; by hand `menu_.bodyChoices = {rowBox, columnBox}` |
| Tab step | LB/RB (PgUp/PgDn, a stick's LP/HP) on a strip of your own | `if (int step = TabDelta(action)) { current = StepTab(current, step, count); action = {}; }` |
| Shortcut hints | Hints for X, Y, View, Delete, LB, RB that the device has, each a button | `ShortcutHints({{MenuInput::Options, label}})`, one `ShortcutHint(press, label)`, one glyph `MenuShortcutGlyph(press)` |

`MenuShortcuts` (ControllerNavigation.hxx) is the one shortcut table: each
press with its Xbox button, a stick's fight function (LP and HP for the tabs),
its keyboard key and the glyphs. `ShortcutButtons` resolves a row for one
device, never on the device's Select or Back buttons (on a stick, the
physical buttons whose fight function is LP or HP, `input::MenuButtons`), and
both the pad's input and the glyphs read it.

### Moving on a board

A screen takes part by drawing its rows itself: every box a body hands to
`PressEntry` is placed (`MenuNavigation::Place(id, MenuRect)`), and on the next
frame the D-pad moves from a placed focus to the placed row nearest in that
direction (its centre past the focus's centre, and either overlapping the
focus across or at least as far past its edge as it is off to the side;
nearest by the gap along the axis plus twice the offset across; ties to the
earlier row). No row that way: the focus stays, or, on a board that set
`nav.SetEdge(MenuEdge::Flow)` (Create, the welcome: panels side by side that
read as one column), goes on to the next placed row in row order (Down,
Right) or the previous (Up, Left). A
GameMenu list draws its own cards and places nothing, so lists, grids and
their footers keep moving by row order (a flyout list too tall for its
height is drawn in two columns and still moves in reading order). The
shell's own rows (`MenuEntry::footer`: Retry saving) belong to no body:
`GameMenu::DrawBody` hands the body the rest and draws them as cards in a band
under it, placed, so Down from the body's lowest row reaches them; a screen
whose only placed row is one of them (a body that places nothing) moves by
row order. Before the geometry come a row's
explicit `left/right/up/down` (only on a screen that places nothing: a
GameMenu list, Home's player card, Rankings) and a value row's Left/Right,
which adjust it. A row that is not drawn is not placed, so
the pad reaches exactly what the mouse can. Do not wire neighbours by id on a
board; draw the rows where they belong.

### Tabs

`StepTab(current, delta, count)` (MenuNavigation.hxx) is the one stepping
rule: it wraps at both ends, and from no current tab RB goes to the first and
LB to the last. `GameMenu::tabs` steps with it; a screen without
`GameMenu::tabs` gets LB/RB as a Shortcut whose `TabDelta` is -1 or 1, for a
body with a strip of its own to take (and clear); a step nobody takes is
dropped before Draw returns. A tab or a Settings panel opens through
`ApplicationShell::ShowTab`: the panel's tab takes the place of the tab shown,
focused on the panel's head, and the panel goes over it, so Back from a panel
opened on another's rail lands on its own head.

## Rules for a new screen

1. Compose the screen from the kit. If a part is missing, add it to the kit
   (short doc comment above it, in the V2.hxx style) and use it from at least
   two screens. Do not draw a button, panel, heading or row by hand.
2. Rows stay `MenuEntry`. Navigation and speech read the
   same rows; the board only decides where each row is drawn. Every box that
   is a row goes through `PressEntry`, which is also how the D-pad finds it
   (Moving on a board).
3. No colour literals and no `palette::` colours on a screen: use
   `v2::Colors()`.
4. Every line of text goes through `v2::Text`, `v2::Fit` or a part that uses
   them, never through `PushClipRect` alone, so the lab's text probe sees what
   is cut.
5. One narrow test: `v2::Narrow(ImGui::GetWindowWidth(), s)`.
6. Sizes follow Kit.dc.html: buttons 48 px with a 17 px heading label, rows
   48 px with the label 14 px in, headings 14 px muted, the words card 16/15 px.
7. Check it: `lab/uilab-v2-check.sh <worktree> <name> <out-dir>` renders
   every V2 screen at four sizes and fails on cut text, text taller than its
   row and overlapping cards. Add the screen to its case list.
