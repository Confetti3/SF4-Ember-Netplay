# Replay takeover

While the game's Battle Log plays a replay, the player can take one fighter and play on from that moment. The other fighter either follows the replay or becomes a training dummy that stands, blocks or blocks low. A takeover lasts until its round is over. Retry goes back to the moment it began, and Back to the replay plays on from there as recorded. SF4 only for now; SFV gets it through the next library branch.

## Controls

| | Keyboard | Pad (XInput) |
|---|---|---|
| Take Player 1 / Player 2 | F6 / F7 | L3 / R3 |
| What the other fighter does (replay, stands, blocks, blocks low) | F8 | Up |
| Retry (taking over) | F6 | Back, tapped |
| Back to the replay (taking over) | F7 | Back, held half a second |

Taking over works in the fight under Ember's controls, paused or not, and starts at 1x. While the player has a fighter, every pad button except Back belongs to the fighter, so the transport's pad controls stand down. Their keys are dropped too, with no "works during the fight" chip. The player's own controller is the one the game reads as Player 1's, for either side taken. The strip at the bottom shows the takeover's controls, and its place line names the fighter taken. Once the round is over it stays up with Retry lit.

## How it works

The spike (branch `spike/replay-takeover`, never merged) found:

- The recorder writes its recorded words over what the battle reads while it plays. With its mode set to stopped (`ReplaySystem+0x708` = 0), `fSystem::ReadButtons` decides what both fighters do.
- `fSystem::SaveState` keeps the recorder, its mode and cursor included. A load is exact, and 50 loads in a row left both fighters where the save put them.
- The recorder's cursor at the save names the record the first update after a load reads. Feeding both sides the recorded words from there (`cursor + updates since`) plays the recording again exactly.

So (`sf4e__ReplayTakeover.cxx`):

- **Take over:** before a battle update, read the round's recording from the recorder's own stream (`ReplayInputs::ReadStream`, the file's record format), refusing a round with nothing to follow, then save the battle and stop the recorder. From the next update, `ReadButtons` asks the takeover what each side reads (`replaytakeover::SourceOf`). The side taken reads the player's own controller. The other side reads the word worked out once before each update (`takeover::OtherWord`): the recorded word for its frame, or the dummy's, which holds away from the player's fighter by their positions (`battle::ReadRoots`).
- **The frame:** the record an update plays is the cursor at the save plus the battle's frames simulated since. A frame the game holds (its pause menu) does not count, and a load puts the count back with the battle.
- **Retry:** load the save and stop the recorder again.
- **Back to the replay:** load the save and leave the recorder playing. The save is then freed.
- **Round over:** the first update out of the fight (a knockout or time) ends the takeover's round. The battle is held there with the freeze bit, so the game's round result, the next round and the end of the battle never run on a fight that no longer matches its recording.
- **Speed:** `replaytransport::Decide` gives a takeover its own route, `Takeover`. It always runs at 1x and ignores the game's Select half speed. The transport resets each time the takeover's phase changes, as it does at a round's start, so the replay plays on at 1x afterwards. A camera jump asked for during a takeover is dropped, and one under way ends.
- **One save:** the takeover's save sits beside the snapshot ring, about 90 memento claims next to the ring's budget, so Retry is always exact. The battle's close frees it, and "Play again" reclaims it.

The commands are the replay controls' own (`replaytransport::Command`). The keys send them (`overlay::SendReplayKeys`), and so does the pad, whose `PadControls` has a takeover mode in which only Back acts. `replaytransport::FateOf` decides each command at the cadence call, and a takeover's acts before the next battle update in the phase it finds then (`replaytransport::StepOf`).

## Files

- `common/ReplayTakeover.hxx`: the model. It covers phases, plans, steps, `Recording` and `OtherWord`.
- `common/ReplayTransport.hxx`: the commands and their kinds, `StepOf`, the `Takeover` route, `FateOf`, the pad's takeover mode and the F6 to F8 keys.
- `common/ReplayInputs.hxx`: `ReadStream` and `PadWord`, the record format both ways.
- `sf4e/sf4e__ReplayTakeover.cxx`: the save, the recorder, the round's end and the words the battle reads.
- `sf4e/sf4e__Pad.cxx`: `ReadButtons` reads each side from where the takeover says.
- `ui/ReplayPlaybackHud.cxx`: the strip's takeover controls.

Tests: `ReplayTakeoverTest`, `ReplayTransportTest`, and the in-game `-Test takeover` (`sf4e__SelfTest__Takeover.cxx`, listed in its header). The in-game test plays the fighter taken itself from the replay's file through `selftest::MatchInput`, and checks these against frame-by-frame positions:

- a followed takeover equals the recording
- the player's input reaches the fighter
- Retry is exact
- the dummy blocks away and its low block is another stance
- the battle holds at the round's end, and Retry and Back work from there

## Open points

- The pad's directions and Back are XInput's physical bits (`TrainingPad.hxx`). DirectInput pads cannot take over by pad, only by keyboard.
- Dummy recording slots (the Training dummy's saved recordings) are not offered yet. They need Training's recording banks moved out of `TrainingRuntime.cxx`.
