# SF4 Ember Netplay v1.2.0

v1.2.0 adds more training controls, Practice rooms and a replay library with video export. You can train while waiting for a match, use the pad in the training menu, and read more detail on the frame meter. Auto input delay follows each game's ping, spectating distant players works again, and new settings reduce input and display delay.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **Everyone in a room needs 1.2.0.** Different builds cannot join each other's rooms, including 1.1.2 and the 1.2.0 Nightlies.
- **From 1.1.2, use the updater.** Ember offers 1.2.0 in game. You can also download the zip from the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.2.0) and extract it over your Ember folder with SF4 closed. Your settings carry over.
- **New installs can use setup.exe.** The installer is also available on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.2.0).
- **Recordings saved on a Nightly may need mirroring.** They are read as recorded with the dummy on the right. If one comes out backwards, use Mirror selected recording in the training controls.

## Changes since v1.1.2

### Training

- **More detail on the frame meter.** Startup, active and recovery frames have separate colours, with marks for hits, button presses and meaty frames. A new angled view sits beside the flat bars, and either shows 60, 90 or 120 frames. Frame advantage and the bars keep the last exchange until the next one replaces it. (Hit, press and meaty marks: FRaccie)
- **Choose the training HUD size and parts.** The HUD page in F6 offers Small, Standard and Large, and lets you hide individual parts. Frame data shows each fighter's last move and recovery, with an optional Recovery readout on the HUD. The shortcut strip lists the training keys and shows when recording or playback is running. (HUD page and recording strip: FRaccie)
- **Training controls work from the pad.** Hold Back (View or Select), then press Start within half a second to open or close F6. Use the D-pad and A/B on an Xbox pad, or mapped LP/LK on DirectInput. Back alone resets the position on a tap and saves it on a hold.
- **Save and reset the training position.** F11 saves both fighters' positions, health, gauges and dummy state; F2 restores them. You can change those keys in Position. Reset restarts a playback and leaves a recording running. (FRaccie, PR #36)
- **Record the dummy without extra setup.** Player 2 needs no controller and the dummy no longer needs to be set to Human. Eight slots hold up to two minutes each, can be saved and loaded by name, and play once or looped. Recordings follow the dummy's facing when sides switch. Playback continues while the training controls are open.
- **Set a dummy reply.** Choose a preset, a recording or typed moves for wake-up, after blocking or after a dropped combo. Set the timing and chance of a reply, or vary the dummy's standing and crouching stance. Typed replies accept numpad notation, motion names such as `qcf` and `dp`, charge moves, held directions and waits. Replies follow the dummy's facing, and After blocking reacts to a real block. (FRaccie, PR #36)
- **Player 2's input history shows Player 2.** After a pad was handed back to Player 1, Player 2's input history no longer copies Player 1's presses. (FRaccie)

### Rooms and spectating

- **Train while waiting in a room.** Wait in Training keeps your seat or queue place and shows your room, table and unread messages under the timer. When someone sits down opposite you, a short notice takes you back to the table, where you have 15 seconds to ready. Enter, or View on an Xbox pad, goes at once. Others in the room see you as In Training. Auto-ready from Training is in Settings and starts off. (FRaccie)
- **Create a Practice room.** Choose Practice room when creating a private room. Every game refills health and gauges, has no KO and uses a 9999 second timer. The frame meter is always shown. Practice starts off each time you create a room and is not saved; to return to normal matches, close the room and create another. Public and tournament rooms cannot be Practice rooms.
- **End practice from Ember.** During a Practice game, Start or F10 opens Ember on End practice. Confirm to return the table to Waiting without awarding a result.
- **Use the frame meter in private matches and replays.** Frame meter in matches is optional for normal private games, including games you watch. It is unavailable in public rooms. (FRaccie)
- **Spectating distant players no longer drops at the start.** Both ends of a spectator connection now finish syncing, and a ping spike no longer drops a healthy spectator. If a stream cannot start within 30 seconds, you see "The spectator stream was dropped" instead of waiting forever.
- **Rooms keep working after a spectator leaves before a match.** Fixed the room freezing with "Room recovery state could not be applied". You can also keep typing in Chat while the room catches up after a game; the message sends when the room is ready.

### Replays

- **Every Ember match is archived when it ends.** Replays are kept as `.emberreplay` files, so long sessions no longer lose older matches when the game's slots fill. Watched matches are saved too by default; turn off Save matches you watch in Settings, Training and replays to keep only games you play. The game's own Battle Log now keeps the last 30 Versus replays instead of 10. (FRaccie)
- **Browse replays from Home.** Watch an archived match, add it to the game's Battle Log, or read both players' inputs and stats by round. Rows show fighter portraits, match details and Seen or Spectated marks. (FRaccie, PRs #27 and #30)
- **Control replay playback.** Pause, step one frame, or slow to half or quarter speed from the keyboard or an Xbox pad. Replays started from Ember can also show the frame meter and both players' recorded inputs.
- **Export a replay as an .mp4.** Choose player names, a match caption, the set score and whether to include the frame meter. Export shows progress, can be cancelled, and keeps running behind another window. (FRaccie)

### Input and display

- **Zero input delay is available again.** Manual delay now runs from 0 to 10 frames. Auto stays between 1 and 4 and never chooses zero.
- **Auto delay follows each game's ping.** After a game with at least ten seconds of ping readings, Auto adjusts the next game's delay against the same opponent, up or down. Its bands remain 1 frame up to 80 ms, 2 up to 150 ms, 3 up to 220 ms and 4 above that. Manual delay stays as chosen, and delay never changes during a fight.
- **Two experimental input lag options.** Lower input lag removes one frame of the game's built-in delay; online, both fighters must enable it. Less keyboard lag removes the keyboard's extra frame on your own PC. Both start off, under Player & controller, based on WAZAAAAA's findings in [#77](https://github.com/Confetti3/SF4-Ember-Netplay/issues/77).
- **Faster display starts on by default.** The game draws without its separate render thread, removing one frame of display delay. If your PC drops below 60 fps, turn it off under Player & controller and restart the game. Each player can choose independently. Your `config.ini` is unchanged, and turning it off restores the game's RenderingThread setting.
- **Keep both games in sync between rounds.** Fixed between-round updates running outside the match's rollback timing. (JimboFGC, [PR #41](https://github.com/Confetti3/SF4-Ember-Netplay/pull/41))

### Menus, updates and reports

- **The pad works in Ember while alt-tabbed.** Start opens Ember at the main menu even when another window is in front. A Training call or an opponent readying also plays a chime and flashes the taskbar button. Keyboard and mouse still need the game in front.
- **Home has Replays, and Ember ID is under Online play.** Ready tournament matches appear at the top of Home while you are outside a room.
- **Choose an update channel and read What's new.** The Updater window, opened from Help & about or by running Updater.exe with the game closed, offers Stable, Beta and Nightly and shows the offered version's notes. Home shows your version and channel. The updater shows its progress in its own window and waits for files still in use, naming the file. (Update progress: FRaccie, PR #29)
- **Problem reports are opt-in.** After a crash, Ember asks before sending a report. Settings, Problem reports has Send problem reports (off by default) and a list of sent reports. Report a problem, under Help & about, shows a preview before anything is sent, and a crash dump is included only if you choose it.
- **Desync reports include more detail.** When a match ends with "the two games diverged", `sf4e.log` records the frames leading up to the split. Send both players' logs.
- **Russian menu hints use the translator's wording.** Online play, Profile, Ember ID, Settings and Play offline now use the supplied short descriptions.

## Known issues

- **Picking your fighter, costume, colour or Ultra does not count as activity for the 90 seconds.** A chat message does count.
- **Spectators cannot stop watching until the game ends** ([#23](https://github.com/Confetti3/SF4-Ember-Netplay/issues/23)).
- **A message sent just as its sender leaves can be missed.** Messages already received stay in Chat.
- **A two-player private room cannot survive its host's game crashing.** The remaining player needs a new room. Leaving normally is fine.
- **Browser links need a fallback on Linux and Steam Deck.** Copy the link and paste it into Ember instead of using Open in Ember.
- **"Practice ended" also appears when a player's link drops.** It does not distinguish a dropped connection from choosing End practice.

Report problems in [Discord](https://discord.gg/uPNqF5A5uq) with `sf4e.log` and `launcher.log` from `%APPDATA%\sf4e\logs`, the time it happened and what you were doing. For installer or update problems, include `%TEMP%\sf4-netplay-update.log` too.

## Testing

The full build passed all 151 automated tests. The networking helper tests passed. Every change went through code review until it was approved.

In game, the same code was tested as a Nightly build, and this build opened and played a public room on the 1.2.0 server.