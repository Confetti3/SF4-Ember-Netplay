# SF6 room layout reference

Use Capcom's Custom Room composition as the reference for Ember's room screen: four stacked battle slots on the left, members above chat on the right. This note records observed reference material, not a claim of completed Ember implementation.

Research date: September 8, 2026. Source: the Custom Room section of [Capcom's Fighting Ground page](https://www.streetfighter.com/6/en-us/mode/fightingground/). These are official promotional screenshots, not a verified capture of the latest installed SF6 release. Do not import SF6 artwork into the package; use Ember's existing SF4 character portraits.

## Match the room composition

The following is visual observation of [Capcom's room screenshot](https://www.streetfighter.com/6/assets/images/mode/fg/custom_img01-en.jpg), inspected directly in a browser:

| Region | Observed layout | Ember adaptation |
| --- | --- | --- |
| Header | Centered room title; privacy and spectating summary beneath | Room name and quiet connection/rules summary |
| Left half | Four numbered, vertically stacked battle slots, not a 2-by-2 grid | Four persistent table cards in the same order |
| Slot header | Battle type on left, concise rules on right | Table number, match phase, and current rules |
| Occupied slot | Two player banners around a central VS; cropped character head/shoulder portraits | Native SF4 character portraits with player names and readiness |
| Empty seat | Clearly labeled vacant player banner | Explicit empty-seat label, not a fabricated player |
| Right upper region | Scrollable member rows, capacity above; player banners include portraits and compact status | Stable member list with character portrait and queue/watch/local status |
| Right lower region | Separate chat panel | Recent chat with explicit compose action |
| Footer | Persistent confirm, back, battle settings, and menu prompts | Actual glyphs and contextual actions |

The selected battle slot has a bright perimeter highlight. Members currently fighting show their battle-slot number and status in the member row. Preserve this hierarchy in Ember's charcoal/ivory/orange styling rather than copying the reference's purple palette.

## Match the setup interaction

[Capcom's mode settings screenshot](https://www.streetfighter.com/6/assets/images/mode/fg/custom_img02-en.jpg) shows three horizontal categories: mode, rules, and room settings. The active category contains labeled left/right value rows. Mode and all four battle slots are independently named. Restore defaults and Create Room are separate actions below the settings region. A one-line explanation and button prompts remain at the bottom.

[Capcom's room settings screenshot](https://www.streetfighter.com/6/assets/images/mode/fg/custom_img03-en.jpg) uses the same layout for capacity, reserved slots, privacy, spectating, connection quality, passcode, and comments. Its selected row has a full-width highlight. These screenshots support separating setup categories from the live room overview; they do not establish which additional SF6 features Ember should implement.

## Keep facts separate from adaptation

Capcom's own representative confirms that SF6 Custom Rooms are separate from Battle Hub, allow up to sixteen participants, and have four cabinets configurable for One on One, Extreme Battle, or Training. Ember should reuse the room arrangement without inventing support for SF6-only modes. [Capcom-authored showcase article](https://blog.playstation.com/2023/04/20/street-fighter-6-showcase-new-gameplay-details-future-fighters-revealed-and-demo-launched/)

The inspected images do not show the menu opened after selecting a cabinet. Keep Ember's existing validated queue, watch, ready, leave, and host command interfaces; do not claim an exact SF6 action-menu reproduction. Likewise, narrow/high-DPI stacking is an Ember accessibility adaptation, not something demonstrated by these landscape reference images.

## Reproduce the reference check

Open the Fighting Ground page's Custom Room section, then inspect its three linked images above. The page's current JavaScript references `custom_img01-en.jpg`, `custom_img02-en.jpg`, and `custom_img03-en.jpg` under `/6/assets/images/mode/fg/`. Browser inspection succeeded even though the web text reader rejected the image URLs. No media was downloaded, no SF6 assets were added to the package, and no game process was used.
