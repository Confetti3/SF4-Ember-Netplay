# Accessibility: Ember's speech

Ember can read its own menus aloud, so a player who cannot see the screen
can set up Ember, find a room and start a match. This guide says how to
turn speech on, which button to use, what Ember reads and when, and what to
do when you hear nothing.

Each section has a recording of itself, read by the same voice Ember uses.

## Turn speech on

[Listen to this section](audio/accessibility/turn-speech-on.ogg)

Open Ember's menu: F10 on the keyboard, or Start on a controller. Go to
Settings, then Accessibility, and set Speech to On with Right.

Ember then says how speech works, once. It also says this the first time a
menu opens with speech on, so a player who has never seen the menu hears it.

On the Steam Deck, the first time speech is on, Ember downloads a natural
voice for your language (about 90 MB) and keeps it. Until it is ready, the
Steam Deck's own voice speaks. Ember tells you when the download starts, when
the natural voice is ready, and if the download failed.

## The accessibility button

[Listen to this section](audio/accessibility/accessibility-button.ogg)

One button asks Ember for more about what you are on. It is the same on
every device:

- Controller: RT (R2 on a PlayStation controller, ZR on a Nintendo one).
- Arcade stick and leverless: the same RT input, the fourth button of the
  bottom row in the Xbox layout.
- Keyboard: F2.

Press it once to hear what the focused item does. Press it again on the same
item to stop. F1 on the keyboard also stops speech.

If you set RT as your confirm or back button, it is not the accessibility
button.

## What Ember reads

[Listen to this section](audio/accessibility/what-ember-reads.ogg)

Ember reads its menus short, and only what changed.

- A new screen: its name.
- Moving to an item: its name. For a setting you change, its value too,
  such as "Lower input lag, On".
- Changing a value: the new value.
- A question you opened, such as leaving a room: the question, then the
  button you are on. Moving says each button.
- The description of an item: only when you press the accessibility button,
  unless you turn on Read descriptions automatically.

Moving on always cuts off what Ember was saying, so you can move fast.

## Learning the button

[Listen to this section](audio/accessibility/learning-the-button.ogg)

Until you have used the accessibility button three times, Ember reminds you
of it: an item that has a description says "RT for more" after its name, and
each screen says "RT for help on any item" the first time it opens. Ember
names your own button, such as R2 or F2.

After that, a soft tick after an item's name tells you it has a description.

[Listen to the tick](audio/accessibility/tick.ogg)

You can keep the reminder on, or turn it off, with Remind me of the describe
button.

## Notices

[Listen to this section](audio/accessibility/notices.ogg)

Some notices open by themselves, for example when an update is ready. Ember
then plays a short chime and says "Notice. Press RT to hear it." Press the
accessibility button to hear the notice and its buttons. Move to choose a
button and confirm as usual.

[Listen to the chime](audio/accessibility/chime.ogg)

A notice that opens because of what you did, such as the one that says how
speech works, is read at once.

## Accessibility settings

[Listen to this section](audio/accessibility/settings.ogg)

All of these are in Settings, Accessibility. All but Speech wait for Speech
to be on.

- Speech: On or Off.
- Speech rate: how fast the voice speaks, from -10 to 10. 0 is its normal
  pace.
- Speech volume: how loud the voice speaks.
- Read descriptions automatically: reads an item's description by itself
  when you stay on it for a moment. Off by default.
- Read button hints: reads which of your buttons does what on the item.
  Off by default.
- Read position: says an item's place in its list, such as 3 of 12.
  Off by default.
- Remind me of the describe button: Until used 3 times, On, or Off.
- Description tick: the soft tick after an item that has a description.
- Notice chime: the chime before a notice that opened by itself.
- Chime volume: how loud the chime plays.
- Voice: on the Steam Deck, which voice speaks, and the natural voice's
  download.

## The Steam Deck's screen reader

[Listen to this section](audio/accessibility/steam-screen-reader.ogg)

The Steam Deck has its own screen reader, in Steam's Settings, Accessibility.
It reads Steam, not Ember's menus, so turn on Ember's speech as well. Ember
never stops the Steam Deck's screen reader; the two can speak at once, so
pause in Ember while Steam is speaking.

## Troubleshooting

[Listen to this section](audio/accessibility/troubleshooting.ogg)

- You hear nothing on Windows. Ember says on screen that Windows has no voice
  to speak with. Add one in Windows Settings, Time and language, Speech.
- You hear nothing on the Steam Deck or Linux. Ember says on screen that it
  could not reach speech-dispatcher. The Steam Deck has it; on another Linux,
  install speech-dispatcher and python3.
- The natural voice could not be downloaded. Ember says so and the system
  voice speaks. Ember tries again the next time it starts. Check that the
  Steam Deck is online; the download comes from github.com and
  huggingface.co.
- Offline: the system voice speaks until the natural voice has been
  downloaded once. After that it works offline.
- Japanese, Korean and Chinese have no natural voice yet, so the system
  voice speaks them.

## Credits

The natural voices are Piper voices (MIT licence) trained on free recordings:
LJ Speech (public domain) for English, and the voices listed in Ember's
accessibility design document for the other languages. The French voice
uses the SIWIS French Speech Synthesis Database (CC BY 4.0).

Other languages: this guide and its recordings are in English only for now.
