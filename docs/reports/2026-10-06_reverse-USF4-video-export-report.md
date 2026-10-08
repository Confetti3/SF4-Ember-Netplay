# Exporting a replay as a video file (reverse engineering notes)

Analysis date: 6 October 2026, on the installed Steam `SSFIV.exe`, with a capstone script over the raw image. Nothing here has been run in the game. Goal: a video file (picture and sound) of a replay, written from Ember, without an external capture tool.

## The game already has a video encoder

The YouTube upload feature records the replay to a `.wmv` first and uploads that file. The upload side is dead (Google's `gdata` API, OAuth client ID baked in), the recording side is intact.

| Piece | Where | Notes |
| --- | --- | --- |
| `Dimps::Game::RecordToFileController` | singleton `0x4DF7C0`, object at `0xA861D0`, vtable `0x94A108` | What the game events talk to. `+0xC` is the "record this battle" flag, `+0x10` "recording". |
| `RecordToFileControllerWin32` | vtable `0x94A42C`, constructed by `0x4E3A00` | Owns the file: `Begin` (`0x4E3780`) makes `<Documents>\CAPCOM\SUPERSTREETFIGHTERIV\raw_video\` (`SHGetFolderPathA`, `0x780DA0`), names clips `SFIVCLIP…` (`0x4E3C50`), checks free space (`GetDiskFreeSpaceExW`, `0x4E3630`), trims old clips with `FindFirstFileA`/`DeleteFileA` in its `storage-cleanup` job (`0x4E3FD0`, `0x4E3C50`). |
| Movie service | singleton `0x6B5FD0`, vtable `0x9670B0`, created by `0x6B5C00` | The encoder. `0x6B45C0` calls `CoInitialize`, then `WMCreateProfileManager` and `WMCreateWriter` from `WMVCore.DLL` (delay-loaded, the only two imports), builds profile `"QLOC YT Profile"` with a `Video Stream` (`WMV3`, falling back to `WVC1`, VBR: `_VBRENABLED`, `_VBRQUALITY`, `_RMAX`, `_BMAX`) and an `Audio Stream` at 48 kHz (`0xBB80`), and runs the writer on its own thread (`WaitForSingleObject`, critical sections). |

The controller's public entry points, all `thiscall` on the singleton:

| Address | Role |
| --- | --- |
| `0x4DF520(BOOL)` | Set the record flag (`+0xC`). The replay events read it before loading a battle. |
| `0x4DF550` | Start: `0x4DF880` opens the service (`+0x14`), takes the window size (`0x771180`, `0x7716A0`), hooks the sound system (`0x674450`, `0x674E70`, `0x6746A0`) and starts (`+0x18`). |
| `0x4DF590` | Stop: `0x4DF940` stops the service (`+0x1C`), waits for the writer to finish (`+0x34`), releases the sound hook. |
| `0x4DF5B0` | Discard: implementation `+0x28` (`0x4E39E0`) deletes the clip (`0x7813B0` is `DeleteFileA`). |
| `0x4DF5D0` | Storage cleanup (`+0x18`): trims `raw_video` by free space. |
| `0x4DF620` | Busy query (`+0x38`). |

Frames reach the service from the battle system's end of frame (`0x5DBAA0`, through `service+0x6C`) and the application's present loop (`0x40427E`, `0x402D20`); the capture of the back buffer is inside the service's own methods (`0x6B3F40`, `0x6B4090`, `0x6B53B0`, Direct3D calls through the device's vtable). Sound comes through the sound system hook the start path installs, so the file carries the game's audio.

## How the upload path uses it

`BattleLog::ModeEvent` has an `Upload` state beside `Select`, `Versus` and `Battle` (`0x46FC40`). Its `UploadEvent` (`0x47ABD0`) discards any previous clip (`0x4DF5B0`), sets the flag (`0x4DF520(1)`), and the mode plays the replay through `Versus` and `Battle` with the recorder running. `LocalReplay::BattleEvent`'s teardown (`0x483A80`) calls `0x4DF590`, then `0x4DF5B0`, then `0x4DF520(0)`: stop, delete, clear. The deletion is why `raw_video` is empty on this PC although the folder exists: the clip only lives between the end of the battle and the upload controller's `waitForUpload` (`0x4E52B0`, `0x4E5B10`).

## What Ember does instead

Ember does not use the game's recorder. Driving it from outside the `Upload` state needs detours on the controller's discard and on `CoInitialize`, the file is a `.wmv` in the game's own profile, and it needs `WMVCore.DLL`. The notes above are kept as a description of the game.

Export video on the Replays screen is Watch now with Ember's own recording of it:

- The render thread reads each finished frame from the device as NV12 (`src/platform/FrameGrab.hxx`) and copies it into shared memory (`src/platform/VideoLink.hxx`).
- A `Launcher.exe` started for the export takes the frames out and encodes them with Media Foundation, H.264 and AAC in an `.mp4`, and captures the game's sound itself through WASAPI process loopback (`src/platform/VideoServe.hxx`, `src/platform/VideoEncoder.hxx`). The encoder is in its own process because it keeps more frames in memory than a 32-bit game has room for.
- The replay operation starts the recording when the battle log's Battle state is up and ends it when playback is left (`src/sf4e/sf4e__ReplayStore.hxx`, `src/sf4e/sf4e__ReplayCapture.hxx`).

Costs and limits:

- Real time: the video takes as long as the replay to make, since it is the game rendering it.
- The picture is the game's render target, at its size.
- Sound needs Windows 10 2004 or later; before that the file is silent. N editions without the Media Feature Pack have no encoder, and the export says so.
- Not tried under Proton.
